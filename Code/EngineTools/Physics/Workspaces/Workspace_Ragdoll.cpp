#include "Workspace_Ragdoll.h"
#include "EngineTools/Core/Widgets/InterfaceHelpers.h"
#include "EngineTools/Animation/ResourceDescriptors/ResourceDescriptor_AnimationSkeleton.h"
#include "EngineTools/ThirdParty/pfd/portable-file-dialogs.h"
#include "Engine/Physics/Systems/WorldSystem_Physics.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Animation/AnimationClip.h"
#include "Engine/Animation/AnimationBlender.h"
#include "Engine/Render/Components/Component_SkeletalMesh.h"
#include "Engine/Entity/EntityWorld.h"
#include "Engine/Entity/EntityWorldUpdateContext.h"
#include "Engine/Animation/AnimationPose.h"
#include "System/Math/MathStringHelpers.h"
#include "System/Math/MathHelpers.h"
#include "Engine/Physics/PhysicsScene.h"

//-------------------------------------------------------------------------

namespace EE::Physics
{
    EE_RESOURCE_WORKSPACE_FACTORY( RagdollWorkspaceFactory, RagdollDefinition, RagdollWorkspace );

    //-------------------------------------------------------------------------

    class [[nodiscard]] ScopedRagdollSettingsModification : public ScopedDescriptorModification
    {
    public:

        ScopedRagdollSettingsModification( RagdollWorkspace* pWorkspace )
            : ScopedDescriptorModification( pWorkspace )
            , m_pRagdollWorkspace( pWorkspace )
        {}

        virtual ~ScopedRagdollSettingsModification()
        {
            if ( m_pRagdollWorkspace->IsPreviewing() )
            {
                m_pRagdollWorkspace->m_refreshRagdollSettings = true;
            }
            else // Any change should update the profile working copy (since we might have added/removed bodies)
            {
                m_pRagdollWorkspace->UpdateProfileWorkingCopy();
            }
        }

    private:

        RagdollWorkspace*  m_pRagdollWorkspace = nullptr;
    };

    //-------------------------------------------------------------------------

    static void DrawTwistLimits( Drawing::DrawContext& ctx, Transform const& jointTransform, Radians limitMin, Radians limitMax, Color color, float lineThickness )
    {
        EE_ASSERT( limitMin < limitMax );

        Vector const axisOfRotation = jointTransform.GetAxisX();
        Vector const zeroRotationVector = jointTransform.GetAxisY();

        constexpr float const limitScale = 0.1f;
        constexpr uint32_t const numVertices = 30;
        TInlineVector<Vector, numVertices> vertices;
        
        Radians const deltaAngle = ( limitMax - limitMin ) / numVertices;
        for ( int32_t i = 0; i < numVertices; i++ )
        {
            auto rotatedVector = Quaternion( axisOfRotation, limitMin + ( deltaAngle * i ) ).RotateVector( zeroRotationVector );
            vertices.push_back( jointTransform.GetTranslation() + ( rotatedVector * limitScale ) );
            ctx.DrawLine( jointTransform.GetTranslation(), vertices.back(), color, lineThickness );
        }

        for ( auto i = 1; i < numVertices; i++ )
        {
            ctx.DrawLine( vertices[i-1], vertices[i], color, lineThickness);
        }
    }

    static void DrawSwingLimits( Drawing::DrawContext& ctx, Transform const& jointTransform, Radians limitY, Radians limitZ, Color color, float lineThickness )
    {
        Vector const axisOfRotationY = jointTransform.GetAxisY();
        Vector const axisOfRotationZ = jointTransform.GetAxisZ();
        Vector const zeroRotationVector = jointTransform.GetAxisX();

        constexpr float const limitScale = 0.2f;
        constexpr uint32_t const numVertices = 30;
        TInlineVector<Vector, numVertices> vertices;

        Radians const startAngleY = -( limitY / 2 );
        Radians const deltaAngleY = limitY / numVertices;
        for ( int32_t i = 0; i < numVertices; i++ )
        {
            auto rotatedVector = Quaternion( axisOfRotationY, startAngleY + ( deltaAngleY * i ) ).RotateVector( zeroRotationVector );
            vertices.push_back( jointTransform.GetTranslation() + ( rotatedVector * limitScale ) );
            ctx.DrawLine( jointTransform.GetTranslation(), vertices.back(), Colors::Lime, lineThickness );
        }

        vertices.clear();

        Radians const startAngleZ = -( limitZ / 2 );
        Radians const deltaAngleZ = limitZ / numVertices;
        for ( int32_t i = 0; i < numVertices; i++ )
        {
            auto rotatedVector = Quaternion( axisOfRotationZ, startAngleZ + ( deltaAngleZ * i ) ).RotateVector( zeroRotationVector );
            vertices.push_back( jointTransform.GetTranslation() + ( rotatedVector * limitScale ) );
            ctx.DrawLine( jointTransform.GetTranslation(), vertices.back(), Colors::RoyalBlue, lineThickness );
        }
    }

    static int32_t GetParentBodyIndex( Animation::Skeleton const* pSkeleton, RagdollDefinition const& definition, int32_t bodyIdx )
    {
        EE_ASSERT( bodyIdx >= 0 && bodyIdx < definition.m_bodies.size() );

        int32_t const boneIdx = pSkeleton->GetBoneIndex( definition.m_bodies[bodyIdx].m_boneID );

        // Traverse the hierarchy to try to find the first parent bone that has a body
        int32_t parentBoneIdx = pSkeleton->GetParentBoneIndex( boneIdx );
        while ( parentBoneIdx != InvalidIndex )
        {
            // Do we have a body for this parent bone?
            StringID const parentBoneID = pSkeleton->GetBoneID( parentBoneIdx );
            for ( int32_t i = 0; i < definition.m_bodies.size(); i++ )
            {
                if ( definition.m_bodies[i].m_boneID == parentBoneID )
                {
                    return i;
                }
            }

            parentBoneIdx = pSkeleton->GetParentBoneIndex( parentBoneIdx );
        }

        return InvalidIndex;
    }

    //-------------------------------------------------------------------------

    RagdollWorkspace::RagdollWorkspace( ToolsContext const* pToolsContext, EntityWorld* pWorld, ResourceID const& resourceID, bool shouldLoadResource )
        : TWorkspace<RagdollDefinition>( pToolsContext, pWorld, resourceID, shouldLoadResource )
        , m_bodyEditorPropertyGrid( pToolsContext )
        , m_solverSettingsGrid( pToolsContext )
        , m_resourceFilePicker( *pToolsContext )
    {
        auto OnPreEdit = [this] ( PropertyEditInfo const& info ) 
        {
            BeginDescriptorModification();
        };
        
        auto OnPostEdit = [this] ( PropertyEditInfo const& info ) 
        {
            EE_ASSERT( m_pActiveUndoableAction != nullptr );
            EndDescriptorModification();
        };

        m_bodyGridPreEditEventBindingID = m_bodyEditorPropertyGrid.OnPreEdit().Bind( OnPreEdit );
        m_bodyGridPostEditEventBindingID = m_bodyEditorPropertyGrid.OnPostEdit().Bind( OnPostEdit );

        //-------------------------------------------------------------------------

        auto OnSolverPreEdit = [this] ( PropertyEditInfo const& info )
        {
            EE_ASSERT( m_pActiveUndoableAction == nullptr );
            BeginDescriptorModification();
        };

        auto OnSolverPostEdit = [this] ( PropertyEditInfo const& info )
        {
            EE_ASSERT( m_pActiveUndoableAction != nullptr );
            auto pSolverSettings = Cast<RagdollDefinition::Profile>( m_solverSettingsGrid.GetEditedType() );
            pSolverSettings->CorrectSettingsToValidRanges();
            EndDescriptorModification();
        };

        m_solverGridPreEditEventBindingID = m_solverSettingsGrid.OnPreEdit().Bind( OnSolverPreEdit );
        m_solverGridPostEditEventBindingID = m_solverSettingsGrid.OnPostEdit().Bind( OnSolverPostEdit );

        //-------------------------------------------------------------------------

        m_gizmo.SetOption( ImGuiX::Gizmo::Options::AllowScale, false );
        m_gizmo.SetOption( ImGuiX::Gizmo::Options::AllowCoordinateSpaceSwitching, false );
        m_gizmo.SwitchMode( ImGuiX::Gizmo::GizmoMode::Translation );
    }

    RagdollWorkspace::~RagdollWorkspace()
    {
        EE_ASSERT( !IsPreviewing() );
        EE_ASSERT( m_pSkeletonTreeRoot == nullptr );
        EE_ASSERT( !m_skeleton.IsSet() || m_skeleton.IsUnloaded() );

        m_bodyEditorPropertyGrid.OnPreEdit().Unbind( m_bodyGridPreEditEventBindingID );
        m_bodyEditorPropertyGrid.OnPostEdit().Unbind( m_bodyGridPostEditEventBindingID );
        m_solverSettingsGrid.OnPreEdit().Unbind( m_solverGridPreEditEventBindingID );
        m_solverSettingsGrid.OnPostEdit().Unbind( m_solverGridPostEditEventBindingID );
    }

    //-------------------------------------------------------------------------
    // Workspace Lifetime
    //-------------------------------------------------------------------------

    void RagdollWorkspace::Initialize( UpdateContext const& context )
    {
        TWorkspace::Initialize( context );

        //-------------------------------------------------------------------------

        m_bodyEditorWindowName.sprintf( "Body Editor##%u", GetID() );
        m_bodyEditorDetailsWindowName.sprintf( "Details##%u", GetID() );
        m_profileEditorWindowName.sprintf( "Profile Editor##%u", GetID() );
        m_previewControlsWindowName.sprintf( "Preview Controls##%u", GetID() );

        //-------------------------------------------------------------------------

        LoadSkeleton();
    }

    void RagdollWorkspace::Shutdown( UpdateContext const& context )
    {
        // Stop preview
        if ( IsPreviewing() )
        {
            StopPreview();
        }

        // Unload preview animation
        if ( m_previewAnimation.IsSet() && !m_previewAnimation.IsUnloaded() )
        {
            UnloadResource( &m_previewAnimation );
        }

        //-------------------------------------------------------------------------

        UnloadSkeleton();
        m_activeProfileID = StringID();
        TWorkspace<RagdollDefinition>::Shutdown( context );
    }

    void RagdollWorkspace::PostUndoRedo( UndoStack::Operation operation, IUndoableAction const* pAction )
    {
        TWorkspace<RagdollDefinition>::PostUndoRedo( operation, pAction );

        if ( IsPreviewing() )
        {
            StopPreview();
        }

        UpdateProfileWorkingCopy();
    }

    void RagdollWorkspace::OnHotReloadStarted( bool descriptorNeedsReload, TInlineVector<Resource::ResourcePtr*, 10> const& resourcesToBeReloaded )
    {
        TWorkspace::OnHotReloadStarted( descriptorNeedsReload, resourcesToBeReloaded );

        if ( IsPreviewing() )
        {
            StopPreview();
        }

        //-------------------------------------------------------------------------

        // If either the skeleton or the descriptor need reload, destroy all editor state
        if ( descriptorNeedsReload || VectorContains( resourcesToBeReloaded, &m_skeleton ) )
        {
            UnloadSkeleton();
        }
    }

    void RagdollWorkspace::OnHotReloadComplete()
    {
        if ( !m_skeleton.IsSet() )
        {
            LoadSkeleton();
        }

        TWorkspace::OnHotReloadComplete();
    }

    //-------------------------------------------------------------------------
    // Workspace UI
    //-------------------------------------------------------------------------

    void RagdollWorkspace::InitializeDockingLayout( ImGuiID dockspaceID ) const
    {
        ImGuiID viewportDockID = 0;
        ImGuiID leftDockID = ImGui::DockBuilderSplitNode( dockspaceID, ImGuiDir_Left, 0.3f, nullptr, &viewportDockID );
        ImGuiID bottomleftDockID = ImGui::DockBuilderSplitNode( leftDockID, ImGuiDir_Down, 0.3f, nullptr, &leftDockID );
        ImGuiID bottomDockID = ImGui::DockBuilderSplitNode( viewportDockID, ImGuiDir_Down, 0.2f, nullptr, &viewportDockID );
        ImGuiID rightDockID = ImGui::DockBuilderSplitNode( viewportDockID, ImGuiDir_Right, 0.2f, nullptr, &viewportDockID );

        // Dock windows
        //-------------------------------------------------------------------------

        ImGui::DockBuilderDockWindow( GetViewportWindowID(), viewportDockID );
        ImGui::DockBuilderDockWindow( m_bodyEditorWindowName.c_str(), leftDockID );
        ImGui::DockBuilderDockWindow( m_bodyEditorDetailsWindowName.c_str(), bottomleftDockID );
        ImGui::DockBuilderDockWindow( m_profileEditorWindowName.c_str(), bottomDockID );
        ImGui::DockBuilderDockWindow( m_descriptorWindowName.c_str(), bottomDockID );
        ImGui::DockBuilderDockWindow( m_previewControlsWindowName.c_str(), rightDockID );
    }

    void RagdollWorkspace::DrawWorkspaceToolbarItems( UpdateContext const& context )
    {
        ImGuiX::VerticalSeparator( ImVec2( 15, -1 ) );

        auto pRagdollDefinition = GetRagdollDefinition();

        //-------------------------------------------------------------------------

        ImGui::BeginDisabled( IsPreviewing() );
        if ( ImGui::BeginMenu( EE_ICON_ACCOUNT_WRENCH" Authoring Tools" ) )
        {
            ImGui::BeginDisabled( !pRagdollDefinition->IsValid() );
            if ( ImGui::MenuItem( "Autogenerate Bodies + joints" ) )
            {
                AutogenerateBodiesAndJoints();
            }

            if ( ImGui::MenuItem( "Autogenerate Body Masses" ) )
            {
                AutogenerateBodyWeights();
            }
            ImGui::EndDisabled();

            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( EE_ICON_RUN" Visualization" ) )
        {
            ImGui::MenuItem( "Show Skeleton", nullptr, &m_drawSkeleton );
            ImGui::MenuItem( "Show Body Axes", nullptr, &m_drawBodyAxes );
            ImGui::MenuItem( "Show Only Selected Bodies", nullptr, &m_isolateSelectedBody );

            ImGui::EndMenu();
        }
        ImGui::EndDisabled();


        // Preview Button
        //-------------------------------------------------------------------------

        ImVec2 const menuDimensions = ImGui::GetContentRegionMax();
        float buttonDimensions = 130;
        ImGui::SameLine( menuDimensions.x / 2 - buttonDimensions / 2 );

        if ( IsPreviewing() )
        {
            if ( ImGuiX::FlatIconButton( EE_ICON_STOP, "Stop Preview", Colors::Red, ImVec2( buttonDimensions, 0 ) ) )
            {
                StopPreview();
            }
        }
        else
        {
            ImGui::BeginDisabled( !pRagdollDefinition->IsValid() );
            if ( ImGuiX::FlatIconButton( EE_ICON_PLAY, "Preview Ragdoll", Colors::Lime, ImVec2( buttonDimensions, 0 ) ) )
            {
                StartPreview( context );
            }
            ImGui::EndDisabled();
        }

        // Gap
        //-------------------------------------------------------------------------

        float const availableX = ImGui::GetContentRegionAvail().x;
        if ( availableX > 140 )
        {
            ImGui::Dummy( ImVec2( availableX - 140, 0 ) );
        }
        else
        {
            ImGuiX::VerticalSeparator();
        }

        // Preview Options
        //-------------------------------------------------------------------------

        if ( ImGui::BeginMenu( EE_ICON_TELEVISION_PLAY" Preview Options" ) )
        {
            ImGui::MenuItem( "Draw Ragdoll", nullptr, &m_drawRagdoll );
            ImGui::MenuItem( "Draw Anim Pose", nullptr, &m_drawAnimationPose );
            ImGui::EndMenu();
        }
    }

    void RagdollWorkspace::DrawViewportToolbarItems( UpdateContext const& context, Render::Viewport const* pViewport )
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 48 );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 4.0f, 4.0f ) );
        if ( ImGui::BeginCombo( "##Help", EE_ICON_HELP_CIRCLE_OUTLINE, ImGuiComboFlags_HeightLarge ) )
        {
            auto DrawHelpRow = [] ( char const* pLabel, char const* pHotkey )
            {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                {
                    ImGuiX::ScopedFont const sf( ImGuiX::Font::Small );
                    ImGui::Text( pLabel );
                }

                ImGui::TableNextColumn();
                {
                    ImGuiX::ScopedFont const sf( ImGuiX::Font::SmallBold );
                    ImGui::Text( pHotkey );
                }
            };

            //-------------------------------------------------------------------------

            if ( ImGui::BeginTable( "HelpTable", 2 ) )
            {
                DrawHelpRow( "Impulse", "Hold Ctrl" );
                DrawHelpRow( "Spawn Collision Object", "Hold Shift" );

                ImGui::EndTable();
            }

            ImGui::EndCombo();
        }
        ImGuiX::ItemTooltip( "Help" );
        ImGui::PopStyleVar();
    }

    void RagdollWorkspace::DrawViewportOverlayElements( UpdateContext const& context, Render::Viewport const* pViewport )
    {
        TWorkspace<RagdollDefinition>::DrawViewportOverlayElements( context, pViewport );

        auto pRagdollDesc = GetRagdollDescriptor();
        if ( !pRagdollDesc->IsValid() )
        {
            return;
        }

        auto pRagdollDefinition = GetRagdollDefinition();
        if ( !pRagdollDefinition->IsValid() )
        {
            return;
        }

        if ( !m_skeleton.IsLoaded() )
        {
            return;
        }

        if ( !m_activeProfileID.IsValid() )
        {
            return;
        }

        //-------------------------------------------------------------------------

        if ( IsPreviewing() )
        {
            Transform previewWorldTransform = m_pMeshComponent->GetWorldTransform();
            auto const gizmoResult = m_gizmo.Draw( previewWorldTransform.GetTranslation(), previewWorldTransform.GetRotation(), *pViewport );
            if ( gizmoResult.m_state == ImGuiX::Gizmo::State::Manipulating )
            {
                gizmoResult.ApplyResult( previewWorldTransform );
                m_pMeshComponent->SetWorldTransform( previewWorldTransform );
            }

            //-------------------------------------------------------------------------

            if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            {
                // Unproject mouse into viewport
                Vector const nearPointWS = pViewport->ScreenSpaceToWorldSpaceNearPlane( ImGui::GetMousePos() - ImGui::GetWindowPos() );
                Vector const farPointWS = pViewport->ScreenSpaceToWorldSpaceFarPlane( ImGui::GetMousePos() - ImGui::GetWindowPos() );

                // Object
                if ( ImGui::IsKeyDown( ImGuiKey_LeftShift ) || ImGui::IsKeyDown( ImGuiKey_LeftShift ) )
                {
                    Vector const initialVelocity = ( farPointWS - nearPointWS ).GetNormalized3() * m_collisionActorInitialVelocity;
                    SpawnCollisionActor( nearPointWS, initialVelocity );
                }
                // Impulse
                else if ( ImGui::IsKeyDown( ImGuiKey_LeftCtrl ) || ImGui::IsKeyDown( ImGuiKey_RightCtrl ) )
                {
                    m_pRagdoll->ApplyImpulse( nearPointWS, ( farPointWS - nearPointWS ).GetNormalized3(), m_impulseStrength );
                }
            }
        }
        else // Editing
        {
            auto drawingCtx = GetDrawingContext();

            // Draw skeleton
            if ( m_drawSkeleton )
            {
                drawingCtx.Draw( *m_skeleton.GetPtr(), Transform::Identity );
            }

            //-------------------------------------------------------------------------

            if ( m_editorMode == Mode::BodyEditor )
            {
                int32_t const numBodies = pRagdollDefinition->GetNumBodies();
                for ( auto i = 0; i < numBodies; i++ )
                {
                    auto const& body = pRagdollDefinition->m_bodies[i];
                    int32_t const boneIdx = m_skeleton->GetBoneIndex( body.m_boneID );
                    EE_ASSERT( boneIdx != InvalidIndex );

                    if ( m_isolateSelectedBody && body.m_boneID != m_selectedBoneID )
                    {
                        continue;
                    }

                    //-------------------------------------------------------------------------

                    Transform const bodyTransform = body.m_offsetTransform * m_skeleton->GetBoneGlobalTransform( boneIdx );
                    drawingCtx.DrawCapsuleHeightX( bodyTransform, body.m_radius, body.m_halfHeight, Colors::Orange, 3.0f );

                    // Draw body axes
                    if ( m_drawBodyAxes )
                    {
                        drawingCtx.DrawAxis( bodyTransform, 0.025f, 2 );
                    }

                    // Draw joint
                    if ( i > 0 )
                    {
                        int32_t const parentBodyIdx = GetParentBodyIndex( m_skeleton.GetPtr(), *pRagdollDefinition, i );
                        int32_t const parentBoneIdx = m_skeleton->GetBoneIndex( pRagdollDefinition->m_bodies[parentBodyIdx].m_boneID );
                        EE_ASSERT( parentBoneIdx != InvalidIndex );
                        Transform const parentBodyTransform = pRagdollDefinition->m_bodies[parentBodyIdx].m_offsetTransform * m_skeleton->GetBoneGlobalTransform( parentBoneIdx );

                        drawingCtx.DrawAxis( body.m_jointTransform, 0.1f, 3 );
                        drawingCtx.DrawLine( body.m_jointTransform.GetTranslation(), parentBodyTransform.GetTranslation(), Colors::Cyan, 2.0f );
                        drawingCtx.DrawLine( body.m_jointTransform.GetTranslation(), bodyTransform.GetTranslation(), Colors::Cyan, 2.0f );
                    }
                }
            }
            else // Joint Editor
            {
                int32_t const numBodies = pRagdollDefinition->GetNumBodies();
                auto pActiveProfile = pRagdollDefinition->GetProfile( m_activeProfileID );

                for ( auto bodyIdx = 1; bodyIdx < numBodies; bodyIdx++ )
                {
                    int32_t const jointIdx = bodyIdx - 1;
                    auto const& body = pRagdollDefinition->m_bodies[bodyIdx];
                    int32_t const boneIdx = m_skeleton->GetBoneIndex( body.m_boneID );
                    EE_ASSERT( boneIdx != InvalidIndex );

                    if ( m_isolateSelectedBody && body.m_boneID != m_selectedBoneID )
                    {
                        continue;
                    }

                    int32_t const parentBodyIdx = GetParentBodyIndex( m_skeleton.GetPtr(), *pRagdollDefinition, bodyIdx );
                    int32_t const parentBoneIdx = m_skeleton->GetBoneIndex( pRagdollDefinition->m_bodies[parentBodyIdx].m_boneID );
                    EE_ASSERT( parentBoneIdx != InvalidIndex );

                    Transform const bodyTransform = body.m_offsetTransform * m_skeleton->GetBoneGlobalTransform( boneIdx );
                    Transform const parentBodyTransform = pRagdollDefinition->m_bodies[parentBodyIdx].m_offsetTransform * m_skeleton->GetBoneGlobalTransform( parentBoneIdx );

                    // Draw joint
                    drawingCtx.DrawAxis( body.m_jointTransform, 0.1f, 4 );
                    drawingCtx.DrawLine( body.m_jointTransform.GetTranslation(), parentBodyTransform.GetTranslation(), Colors::Cyan, 2.0f );
                    drawingCtx.DrawLine( body.m_jointTransform.GetTranslation(), bodyTransform.GetTranslation(), Colors::Cyan, 2.0f );

                    // Draw limits
                    if ( pActiveProfile->m_jointSettings[jointIdx].m_twistLimitEnabled )
                    {
                        DrawTwistLimits( drawingCtx, body.m_jointTransform, Degrees( pActiveProfile->m_jointSettings[jointIdx].m_twistLimitMin ), Degrees( pActiveProfile->m_jointSettings[jointIdx].m_twistLimitMax ), Colors::Red, 1.0f );
                    }

                    if ( pActiveProfile->m_jointSettings[jointIdx].m_swingLimitEnabled )
                    {
                        DrawSwingLimits( drawingCtx, body.m_jointTransform, Degrees( pActiveProfile->m_jointSettings[jointIdx].m_swingLimitY ), Degrees( pActiveProfile->m_jointSettings[jointIdx].m_swingLimitZ ), Colors::Yellow, 1.0f );
                    }
                }
            }

            // Gizmo
            //-------------------------------------------------------------------------

            if ( m_selectedBoneID.IsValid() )
            {
                int32_t const boneIdx = m_skeleton->GetBoneIndex( m_selectedBoneID );
                int32_t const bodyIdx = pRagdollDefinition->GetBodyIndexForBoneID( m_selectedBoneID );
                if ( bodyIdx != InvalidIndex )
                {
                    auto& body = pRagdollDefinition->m_bodies[bodyIdx];

                    if ( m_editorMode == Mode::BodyEditor )
                    {
                        Transform const boneTransform = m_skeleton->GetBoneGlobalTransform( boneIdx );
                        Transform const bodyTransform = body.m_offsetTransform * boneTransform;

                        auto const gizmoResult = m_gizmo.Draw( bodyTransform.GetTranslation(), bodyTransform.GetRotation(), *pViewport );
                        switch ( gizmoResult.m_state )
                        {
                            case ImGuiX::Gizmo::State::StartedManipulating :
                            {
                                BeginDescriptorModification();
                                body.m_offsetTransform = Transform::Delta( boneTransform, gizmoResult.GetModifiedTransform( bodyTransform ) );
                            }
                            break;

                            case ImGuiX::Gizmo::State::Manipulating :
                            {
                                body.m_offsetTransform = Transform::Delta( boneTransform, gizmoResult.GetModifiedTransform( bodyTransform ) );
                            }
                            break;

                            case ImGuiX::Gizmo::State::StoppedManipulating :
                            {
                                body.m_offsetTransform = Transform::Delta( boneTransform, gizmoResult.GetModifiedTransform( bodyTransform ) );
                                EndDescriptorModification();
                            }
                            break;

                            default:
                            break;
                        }
                    }
                    else
                    {
                        auto const gizmoResult = m_gizmo.Draw( body.m_jointTransform.GetTranslation(), body.m_jointTransform.GetRotation(), *pViewport );
                        switch ( gizmoResult.m_state )
                        {
                            case ImGuiX::Gizmo::State::StartedManipulating:
                            {
                                BeginDescriptorModification();
                                gizmoResult.ApplyResult( body.m_jointTransform );
                            }
                            break;

                            case ImGuiX::Gizmo::State::Manipulating:
                            {
                                gizmoResult.ApplyResult( body.m_jointTransform );
                            }
                            break;

                            case ImGuiX::Gizmo::State::StoppedManipulating:
                            {
                                gizmoResult.ApplyResult( body.m_jointTransform );
                                EndDescriptorModification();
                            }
                            break;

                            default:
                            break;
                        }
                    }
                }

                if ( m_isViewportFocused )
                {
                    if ( ImGui::IsKeyPressed( ImGuiKey_Space ) )
                    {
                        m_gizmo.SwitchToNextMode();
                    }
                }
            }
        }
    }

    void RagdollWorkspace::Update( UpdateContext const& context, ImGuiWindowClass* pWindowClass, bool isFocused )
    {
        TWorkspace::Update( context, pWindowClass, isFocused );

        // Lazy creation of editor state
        //-------------------------------------------------------------------------

        if ( m_needToCreateEditorState )
        {
            // Once the skeleton is loaded - create all required editor state
            if ( m_skeleton.IsLoaded() )
            {
                CreatePreviewEntity();
                CreateSkeletonTree();
                ValidateDefinition();

                m_needToCreateEditorState = false;
            }
        }

        // Profile Management
        //-------------------------------------------------------------------------

        auto pRagdollDefinition = GetRagdollDefinition();
        if ( pRagdollDefinition->IsValid() )
        {
            // Ensure we always have a valid profile set
            if ( !m_activeProfileID.IsValid() || pRagdollDefinition->GetProfile( m_activeProfileID ) == nullptr )
            {
                SetActiveProfile( pRagdollDefinition->GetDefaultProfile()->m_ID );
            }
        }

        // UI
        //-------------------------------------------------------------------------

        ImGui::BeginDisabled( IsPreviewing() );
        {
            DrawBodyEditorWindow( context, pWindowClass );
            DrawBodyEditorDetailsWindow( context, pWindowClass );
        }
        ImGui::EndDisabled();

        DrawProfileEditorWindow( context, pWindowClass );
        DrawPreviewControlsWindow( context, pWindowClass );
        DrawDialogs();
    }

    void RagdollWorkspace::PreUpdateWorld( EntityWorldUpdateContext const& updateContext )
    {
        if ( !IsPreviewing() )
        {
            return;
        }

        Seconds const deltaTime = updateContext.GetDeltaTime();

        //-------------------------------------------------------------------------

        if ( updateContext.GetUpdateStage() == UpdateStage::PrePhysics )
        {
            // Update animation time and sample pose
            Percentage const previousAnimTime = m_animTime;
            bool const hasValidAndLoadedPreviewAnim = m_previewAnimation.IsSet() && m_previewAnimation.IsLoaded();
            if ( hasValidAndLoadedPreviewAnim )
            {
                if ( m_isPlayingAnimation )
                {
                    m_animTime = ( m_animTime + deltaTime / ( m_previewAnimation->GetDuration() ) ).GetClamped( m_enableAnimationLooping );

                    // Allow reset
                    if ( !m_enableAnimationLooping && m_animTime == 1.0f )
                    {
                        m_isPlayingAnimation = false;
                    }

                    if ( !updateContext.IsWorldPaused() && m_applyRootMotion )
                    {
                        Transform const& WT = m_pMeshComponent->GetWorldTransform();
                        Transform const RMD = m_previewAnimation->GetRootMotionDelta( previousAnimTime, m_animTime );
                        m_pMeshComponent->SetWorldTransform( RMD * WT );
                    }
                }

                m_previewAnimation->GetPose( m_animTime, m_pPose );
            }
            else
            {
                m_pPose->Reset( Animation::Pose::Type::ReferencePose );
            }

            //-------------------------------------------------------------------------

            Transform const worldTransform = m_pMeshComponent->GetWorldTransform();

            // Draw the pose
            if ( m_drawAnimationPose )
            {
                auto drawingCtx = GetDrawingContext();
                drawingCtx.Draw( *m_pPose, worldTransform );
            }

            // Set ragdoll settings
            if ( m_refreshRagdollSettings )
            {
                m_pRagdoll->RefreshSettings();
                m_refreshRagdollSettings = false;
            }

            // Update ragdoll
            m_pPose->CalculateGlobalTransforms();
            if ( !updateContext.IsWorldPaused() )
            {
                m_pRagdoll->Update( updateContext.GetDeltaTime(), worldTransform, m_pPose, m_initializeRagdollPose );
                m_initializeRagdollPose = false;
            }
        }
        else if ( updateContext.GetUpdateStage() == UpdateStage::PostPhysics )
        {
            auto drawingCtx = GetDrawingContext();

            // Copy the anim pose
            m_pFinalPose->CopyFrom( m_pPose );

            // Retrieve the ragdoll pose
            Transform const& worldTransform = m_pMeshComponent->GetWorldTransform();
            bool const shouldStopPreview = !m_pRagdoll->GetPose( worldTransform, m_pPose );

            // Apply physics blend weight
            Animation::Blender::Blend( m_pFinalPose, m_pPose, m_physicsBlendWeight, nullptr, m_pFinalPose );

            // Draw ragdoll pose
            if ( m_drawRagdoll )
            {
                drawingCtx.Draw( *m_pRagdoll );
            }

            // Update mesh pose
            m_pFinalPose->CalculateGlobalTransforms();
            m_pMeshComponent->SetPose( m_pFinalPose );
            m_pMeshComponent->FinalizePose();

            //-------------------------------------------------------------------------

            UpdateSpawnedCollisionActors( drawingCtx, updateContext.GetDeltaTime() );

            //-------------------------------------------------------------------------

            if ( shouldStopPreview )
            {
                EE_LOG_WARNING( "Physics", "Ragdoll", "Ragdoll destabilized" );
                StopPreview();
            }
        }
    }

    void RagdollWorkspace::DrawDialogs()
    {
        constexpr static char const* const dialogNames[2] = { "Create New Profile", "Rename Profile" };

        auto pRagdollDefinition = GetRagdollDefinition();

        //-------------------------------------------------------------------------

        bool completeOperation = false;
        bool isDialogOpen = m_activeOperation != Operation::None;
        if ( isDialogOpen )
        {
            char const* pDialogName = ( m_activeOperation == Operation::CreateProfile ) ? dialogNames[0] : dialogNames[1];

            ImGui::OpenPopup( pDialogName );
            ImGui::SetNextWindowSize( ImVec2( 400, 90 ) );
            if ( ImGui::BeginPopupModal( pDialogName, &isDialogOpen, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar ) )
            {
                // Validate current name in buffer
                //-------------------------------------------------------------------------

                bool isValidName = strlen( m_profileNameBuffer ) > 0;
                if ( isValidName )
                {
                    StringID const potentialID( m_profileNameBuffer );
                    for ( auto const& profile : pRagdollDefinition->m_profiles )
                    {
                        if ( profile.m_ID == potentialID )
                        {
                            isValidName = false;
                            break;
                        }
                    }
                }

                // Draw UI
                //-------------------------------------------------------------------------

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "Name: " );
                ImGui::SameLine();

                ImGui::SetNextItemWidth( -1 );
                ImGui::PushStyleColor( ImGuiCol_Text, isValidName ? (uint32_t) ImGuiX::Style::s_colorText : Colors::Red.ToUInt32_ABGR() );
                completeOperation = ImGui::InputText( "##ProfileName", m_profileNameBuffer, 256, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter, ImGuiX::FilterNameIDChars );
                ImGui::PopStyleColor();

                ImGui::NewLine();
                ImGui::SameLine( ImGui::GetContentRegionAvail().x - 120 - ImGui::GetStyle().ItemSpacing.x );

                ImGui::BeginDisabled( !isValidName );
                if ( ImGui::Button( "OK", ImVec2( 60, 0 ) ) )
                {
                    completeOperation = true;
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                if ( ImGui::Button( "Cancel", ImVec2( 60, 0 ) ) )
                {
                    m_activeOperation = Operation::None;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }

        // If the dialog was closed (i.e. operation canceled)
        if ( !isDialogOpen )
        {
            m_activeOperation = Operation::None;
        }

        //-------------------------------------------------------------------------

        if ( completeOperation )
        {
            switch ( m_activeOperation )
            {
                case Operation::CreateProfile:
                {
                    StringID const newProfileID = CreateProfile( m_profileNameBuffer );
                    SetActiveProfile( newProfileID );
                }
                break;

                case Operation::DuplicateProfile:
                {
                    StringID const newProfileID = DuplicateProfile( m_activeProfileID, m_profileNameBuffer );
                    SetActiveProfile( newProfileID );
                }
                break;

                case Operation::RenameProfile:
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    auto pProfile = pRagdollDefinition->GetProfile( m_activeProfileID );
                    EE_ASSERT( pProfile != nullptr );
                    pProfile->m_ID = StringID( m_profileNameBuffer );
                    SetActiveProfile( pProfile->m_ID );
                }
                break;

                default:
                {
                    EE_UNREACHABLE_CODE();
                }
                break;
            }

            m_activeOperation = Operation::None;
        }
    }

    //-------------------------------------------------------------------------
    // Resource Management
    //-------------------------------------------------------------------------

    void RagdollWorkspace::LoadSkeleton()
    {
        EE_ASSERT( !m_skeleton.IsSet() || !m_skeleton.WasRequested() );

        auto pRagdollDesc = GetRagdollDescriptor();
        m_skeleton = pRagdollDesc->m_skeleton;
        if ( m_skeleton.IsSet() )
        {
            LoadResource( &m_skeleton );
            m_needToCreateEditorState = true;
        }
        else
        {
            pfd::message( "Invalid Descriptor", "Descriptor doesnt have a skeleton set!\r\nPlease set a valid skeleton via the descriptor editor", pfd::choice::ok, pfd::icon::error ).result();
        }
    }

    void RagdollWorkspace::UnloadSkeleton()
    {
        // Destroy all dependent elements
        DestroyPreviewEntity();
        DestroySkeletonTree();

        // Unload skeleton and clear ptr
        if ( m_skeleton.WasRequested() )
        {
            UnloadResource( &m_skeleton );
        }
        m_skeleton.Clear();

        m_needToCreateEditorState = false;
    }

    void RagdollWorkspace::CreatePreviewEntity()
    {
        EE_ASSERT( m_pDescriptor != nullptr );
        EE_ASSERT( m_skeleton.IsLoaded() );

        // Load resource descriptor for skeleton to get the preview mesh
        auto resourceDescPath = GetFileSystemPath( m_skeleton.GetResourcePath() );
        Animation::SkeletonResourceDescriptor skeletonResourceDesc;
        bool const result = Resource::ResourceDescriptor::TryReadFromFile( *m_pToolsContext->m_pTypeRegistry, resourceDescPath, skeletonResourceDesc );
        EE_ASSERT( result );

        // Create preview entity
        ResourceID const previewMeshResourceID( skeletonResourceDesc.m_previewMesh.GetResourcePath() );
        if ( previewMeshResourceID.IsValid() )
        {
            m_pMeshComponent = EE::New<Render::SkeletalMeshComponent>( StringID( "Mesh Component" ) );
            m_pMeshComponent->SetSkeleton( m_skeleton.GetResourceID() );
            m_pMeshComponent->SetMesh( previewMeshResourceID );

            m_pPreviewEntity = EE::New<Entity>( StringID( "Preview" ) );
            m_pPreviewEntity->AddComponent( m_pMeshComponent );
            AddEntityToWorld( m_pPreviewEntity );
        }
    }

    void RagdollWorkspace::DestroyPreviewEntity()
    {
        if ( m_pPreviewEntity != nullptr )
        {
            DestroyEntityInWorld( m_pPreviewEntity );
            m_pMeshComponent = nullptr;
        }
    }

    void RagdollWorkspace::ValidateDefinition()
    {
        InlineString errorMsg;

        auto pRagdollDesc = GetRagdollDescriptor();
        EE_ASSERT( pRagdollDesc->IsValid() );

        auto pRagdollDefinition = GetRagdollDefinition();
        if ( pRagdollDefinition->IsValid() )
        {
            return;
        }

        // Ensure skeleton matches that of the descriptor
        //-------------------------------------------------------------------------

        if ( !pRagdollDefinition->m_skeleton.IsSet() )
        {
            pRagdollDefinition->m_skeleton = pRagdollDesc->m_skeleton;
        }
        else // Skeleton mismatch
        {
            if ( pRagdollDesc->m_skeleton.GetResourceID() != pRagdollDefinition->m_skeleton.GetResourceID() )
            {
                errorMsg.sprintf( "Skeleton mismatch detected.\r\nDescriptor Skeleton: %s\r\nDefinition Skeleton: %s\r\n\r\nThe definition will now be reset!", pRagdollDesc->m_skeleton.GetResourceID().c_str(), pRagdollDefinition->m_skeleton.GetResourceID().c_str() );
                pfd::message( "Ragdoll Skeleton Mismatch", errorMsg.c_str(), pfd::choice::ok, pfd::icon::error ).result();

                // Remove all bodies
                pRagdollDefinition->m_bodies.clear();

                // Reset all profiles
                for ( auto& profile : pRagdollDefinition->m_profiles )
                {
                    ResetProfile( profile );
                }

                MarkDirty();
            }
        }

        // Ensure we have at least one profile
        //-------------------------------------------------------------------------

        int32_t const numBodies = pRagdollDefinition->GetNumBodies();

        if ( pRagdollDefinition->m_profiles.empty() )
        {
            auto& profile = pRagdollDefinition->m_profiles.emplace_back();
            profile.m_ID = StringID( RagdollDefinition::Profile::s_defaultProfileID );
            profile.m_bodySettings.resize( numBodies );
            profile.m_materialSettings.resize( numBodies );

            if ( numBodies > 1 )
            {
                profile.m_jointSettings.resize( numBodies - 1 );
            }

            m_activeProfileID = profile.m_ID;
            MarkDirty();
        }

        // Validate all profiles
        //-------------------------------------------------------------------------

        for ( auto& profile : pRagdollDefinition->m_profiles )
        {
            if ( !profile.IsValid( numBodies ) )
            {
                errorMsg.sprintf( "Profile %s is invalid. This profile will be reset!", profile.m_ID.c_str() );
                pfd::message( "Invalid Ragdoll Profile", errorMsg.c_str(), pfd::choice::ok, pfd::icon::error ).result();

                // Reset Settings
                ResetProfile( profile );
                MarkDirty();
            }
        }
    }

    //-------------------------------------------------------------------------
    // Bodies
    //-------------------------------------------------------------------------

    void RagdollWorkspace::CreateBody( StringID boneID )
    {
        auto pRagdollDefinition = GetRagdollDefinition();
        EE_ASSERT( pRagdollDefinition->IsValid() && m_skeleton.IsLoaded() );

        if ( pRagdollDefinition->GetNumBodies() >= RagdollDefinition::s_maxNumBodies )
        {
            return;
        }

        ScopedRagdollSettingsModification const sdm( this );

        EE_ASSERT( pRagdollDefinition->GetBodyIndexForBoneID( boneID ) == InvalidIndex );

        int32_t const newBodyBoneIdx = m_skeleton->GetBoneIndex( boneID );
        int32_t const numBodies = pRagdollDefinition->GetNumBodies();
        int32_t bodyInsertionIdx = 0;
        for ( bodyInsertionIdx = 0; bodyInsertionIdx < numBodies; bodyInsertionIdx++ )
        {
            int32_t const existingBodyBoneIdx = m_skeleton->GetBoneIndex( pRagdollDefinition->m_bodies[bodyInsertionIdx].m_boneID );
            if ( existingBodyBoneIdx > newBodyBoneIdx )
            {
                break;
            }
        }

        // Body
        //-------------------------------------------------------------------------

        pRagdollDefinition->m_bodies.insert( pRagdollDefinition->m_bodies.begin() + bodyInsertionIdx, RagdollDefinition::BodyDefinition() );
        auto& newBody = pRagdollDefinition->m_bodies[bodyInsertionIdx];
        newBody.m_boneID = boneID;

        // Calculate initial  body and joint transforms
        Transform boneTransform = m_skeleton->GetBoneGlobalTransform( newBodyBoneIdx );
        Transform bodyGlobalTransform = boneTransform;

        int32_t const firstChildIdx = m_skeleton->GetFirstChildBoneIndex( newBodyBoneIdx );
        if ( firstChildIdx != InvalidIndex )
        {
            auto childBoneTransform = m_skeleton->GetBoneGlobalTransform( firstChildIdx );

            Vector bodyAxisX;
            float length = 0.0f;
            ( childBoneTransform.GetTranslation() - boneTransform.GetTranslation() ).ToDirectionAndLength3( bodyAxisX, length );

            // See if we need to adjust the default radius
            float const halfLength = length / 2;
            if ( newBody.m_radius > halfLength )
            {
                newBody.m_radius = halfLength * 2.0f / 3.0f;
            }

            // Calculate the new half-height for the capsule
            newBody.m_halfHeight = Math::Max( ( length / 2 ) - newBody.m_radius, 0.0f );
            Vector const bodyOffset = bodyAxisX * ( newBody.m_halfHeight + newBody.m_radius );

            // Calculate the global transform for the body
            Quaternion const orientation = Quaternion::FromRotationBetweenNormalizedVectors( Vector::UnitX, bodyAxisX );
            bodyGlobalTransform.SetRotation( orientation );
            bodyGlobalTransform.SetTranslation( boneTransform.GetTranslation() + bodyOffset );

            newBody.m_offsetTransform = Transform::Delta( boneTransform, bodyGlobalTransform );
        }
        else // Leaf bone
        {
            newBody.m_offsetTransform = Transform::Identity;
        }

        // Joint Transform
        newBody.m_jointTransform = Transform( bodyGlobalTransform.GetRotation(), boneTransform.GetTranslation() );

        // Profiles
        //-------------------------------------------------------------------------

        for ( auto& profile : pRagdollDefinition->m_profiles )
        {
            // Mass
            profile.m_bodySettings.insert( profile.m_bodySettings.begin() + bodyInsertionIdx, RagdollBodySettings() );
            float const capsuleVolume = Math::CalculateCapsuleVolume( pRagdollDefinition->m_bodies[bodyInsertionIdx].m_radius, pRagdollDefinition->m_bodies[bodyInsertionIdx].m_halfHeight * 2 );
            profile.m_bodySettings[bodyInsertionIdx].m_mass = capsuleVolume * 985.0f; // human body density

            // Materials
            profile.m_materialSettings.insert( profile.m_materialSettings.begin() + bodyInsertionIdx, RagdollBodyMaterialSettings() ); // human body density

            //-------------------------------------------------------------------------

            int32_t jointInsertionIdx = bodyInsertionIdx - 1;

            // If there are other bodies then we also need to add the joint between the new body and the previous root body
            if ( bodyInsertionIdx == 0 && pRagdollDefinition->m_bodies.size() > 1 )
            {
                jointInsertionIdx = 0;
            }

            // Add the joint settings
            if ( jointInsertionIdx >= 0 )
            {
                profile.m_jointSettings.insert( profile.m_jointSettings.begin() + jointInsertionIdx, RagdollJointSettings() );
            }
        }
    }

    void RagdollWorkspace::DestroyBody( int32_t bodyIdx )
    {
        ScopedRagdollSettingsModification const sdm( this );

        auto pRagdollDefinition = GetRagdollDefinition();
        EE_ASSERT( pRagdollDefinition->IsValid() && m_skeleton.IsLoaded() );
        EE_ASSERT( bodyIdx >= 0 && bodyIdx < pRagdollDefinition->GetNumBodies() );

        // Body
        //-------------------------------------------------------------------------

        pRagdollDefinition->m_bodies.erase( pRagdollDefinition->m_bodies.begin() + bodyIdx );

        // Profiles
        //-------------------------------------------------------------------------

        for ( auto& profile : pRagdollDefinition->m_profiles )
        {
            profile.m_bodySettings.erase( profile.m_bodySettings.begin() + bodyIdx );
            profile.m_materialSettings.erase( profile.m_materialSettings.begin() + bodyIdx );

            int32_t jointIdx = bodyIdx - 1;

            // If this isnt the last body being removed, then removing the first body should also remove the first joint
            if ( bodyIdx == 0 && !profile.m_jointSettings.empty() )
            {
                jointIdx = 0;
            }

            // Remove the relevant joint
            if ( jointIdx >= 0 )
            {
                profile.m_jointSettings.erase( profile.m_jointSettings.begin() + jointIdx );
            }
        }
    }

    void RagdollWorkspace::DestroyChildBodies( int32_t destructionRootBodyIdx )
    {
        ScopedRagdollSettingsModification const sdm( this );

        auto pRagdollDefinition = GetRagdollDefinition();
        EE_ASSERT( pRagdollDefinition->IsValid() && m_skeleton.IsLoaded() );
        EE_ASSERT( destructionRootBodyIdx >= 0 && destructionRootBodyIdx < pRagdollDefinition->GetNumBodies() );

        int32_t const destructionRootBoneIdx = m_skeleton->GetBoneIndex( pRagdollDefinition->m_bodies[destructionRootBodyIdx].m_boneID );

        for ( int32_t bodyIdx = 0; bodyIdx < (int32_t) pRagdollDefinition->m_bodies.size(); bodyIdx++ )
        {
            int32_t const bodyBoneIdx = m_skeleton->GetBoneIndex( pRagdollDefinition->m_bodies[bodyIdx].m_boneID );
            if ( m_skeleton->IsChildBoneOf( destructionRootBoneIdx, bodyBoneIdx ) )
            {
                DestroyBody( bodyIdx );
                bodyIdx--;
            }
        }
    }

    void RagdollWorkspace::AutogenerateBodiesAndJoints()
    {
        ScopedRagdollSettingsModification const sdm( this );

        auto pRagdollDefinition = GetRagdollDefinition();
        EE_ASSERT( pRagdollDefinition->IsValid() && m_skeleton.IsLoaded() );

        // Remove all bodies
        //-------------------------------------------------------------------------

        while ( !pRagdollDefinition->m_bodies.empty() )
        {
            DestroyBody( 0 );
        }

        // Recreate all bodies
        //-------------------------------------------------------------------------
        // Skipping the root bone

        int32_t const numBones = m_skeleton->GetNumBones();
        int32_t const numBodiesToCreate = Math::Min( numBones, (int32_t) RagdollDefinition::s_maxNumBodies );
        for ( auto i = 1; i < numBodiesToCreate; i++ )
        {
            CreateBody( m_skeleton->GetBoneID( i ) );
        }
    }

    void RagdollWorkspace::AutogenerateBodyWeights()
    {
        ScopedRagdollSettingsModification const sdm( this );

        auto pRagdollDefinition = GetRagdollDefinition();
        RagdollDefinition::Profile* pActiveProfile = pRagdollDefinition->GetProfile( m_activeProfileID );
        EE_ASSERT( pActiveProfile != nullptr );

        int32_t const numBodies = pRagdollDefinition->GetNumBodies();
        for ( int32_t i = 0; i < numBodies; i++ )
        {
            float const capsuleVolume = Math::CalculateCapsuleVolume( pRagdollDefinition->m_bodies[i].m_radius, pRagdollDefinition->m_bodies[i].m_halfHeight * 2 );
            pActiveProfile->m_bodySettings[i].m_mass = capsuleVolume * 985; // human body density
        }
    }

    void RagdollWorkspace::DrawBodyEditorWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass )
    {
        ImGui::SetNextWindowClass( pWindowClass );
        if ( ImGui::Begin( m_bodyEditorWindowName.c_str() ) )
        {
            if ( m_pSkeletonTreeRoot != nullptr )
            {
                ImGui::Text( "Skeleton ID: %s", m_skeleton->GetResourcePath().GetFileNameWithoutExtension().c_str() );

                if ( m_editorMode == Mode::BodyEditor )
                {
                    if ( ImGuiX::ColoredButton( Colors::RoyalBlue, Colors::White, "Switch to Joint Editor", ImVec2( -1, 0 ) ) )
                    {
                        m_editorMode = Mode::JointEditor;
                    }
                }
                else
                {
                    if ( ImGuiX::ColoredButton( Colors::Orange, Colors::Black, "Switch to Body Editor", ImVec2( -1, 0 ) ) )
                    {
                        m_editorMode = Mode::BodyEditor;
                    }
                }

                ImGui::Separator();

                EE_ASSERT( m_pSkeletonTreeRoot != nullptr );
                RenderSkeletonTree( m_pSkeletonTreeRoot );
            }
        }
        ImGui::End();
    }

    void RagdollWorkspace::DrawBodyEditorDetailsWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass )
    {
        ImGui::SetNextWindowClass( pWindowClass );
        auto pRagdollDefinition = GetRagdollDefinition();
        if ( m_selectedBoneID.IsValid() )
        {
            int32_t const bodyIdx = pRagdollDefinition->GetBodyIndexForBoneID( m_selectedBoneID );
            if ( bodyIdx != InvalidIndex )
            {
                if ( m_bodyEditorPropertyGrid.GetEditedType() != &pRagdollDefinition->m_bodies[bodyIdx] )
                {
                    m_bodyEditorPropertyGrid.SetTypeToEdit( &pRagdollDefinition->m_bodies[bodyIdx] );
                }
            }
            else
            {
                if ( m_bodyEditorPropertyGrid.GetEditedType() != nullptr )
                {
                    m_bodyEditorPropertyGrid.SetTypeToEdit( nullptr );
                }
            }
        }
        else
        {
            if ( m_bodyEditorPropertyGrid.GetEditedType() != nullptr )
            {
                m_bodyEditorPropertyGrid.SetTypeToEdit( nullptr );
            }
        }

        //-------------------------------------------------------------------------

        if ( ImGui::Begin( m_bodyEditorDetailsWindowName.c_str() ) )
        {
            m_bodyEditorPropertyGrid.DrawGrid();
        }
        ImGui::End();
    }

    void RagdollWorkspace::CreateSkeletonTree()
    {
        TVector<BoneInfo*> boneInfos;

        // Create all infos
        int32_t const numBones = m_skeleton->GetNumBones();
        for ( auto i = 0; i < numBones; i++ )
        {
            auto& pBoneInfo = boneInfos.emplace_back( EE::New<BoneInfo>() );
            pBoneInfo->m_boneIdx = i;
        }

        // Create hierarchy
        for ( auto i = 1; i < numBones; i++ )
        {
            int32_t const parentBoneIdx = m_skeleton->GetParentBoneIndex( i );
            EE_ASSERT( parentBoneIdx != InvalidIndex );
            boneInfos[parentBoneIdx]->m_children.emplace_back( boneInfos[i] );
        }

        // Set root
        m_pSkeletonTreeRoot = boneInfos[0];
    }

    void RagdollWorkspace::DestroySkeletonTree()
    {
        if ( m_pSkeletonTreeRoot != nullptr )
        {
            m_pSkeletonTreeRoot->DestroyChildren();
            EE::Delete( m_pSkeletonTreeRoot );
        }
    }

    ImRect RagdollWorkspace::RenderSkeletonTree( BoneInfo* pBone )
    {
        EE_ASSERT( pBone != nullptr );
        auto pRagdollDefinition = GetRagdollDefinition();

        //-------------------------------------------------------------------------

        StringID const currentBoneID = m_skeleton->GetBoneID( pBone->m_boneIdx );
        int32_t const bodyIdx = pRagdollDefinition->GetBodyIndexForBoneID( currentBoneID );
        bool const hasBody = bodyIdx != InvalidIndex;

        //-------------------------------------------------------------------------

        ImGui::SetNextItemOpen( pBone->m_isExpanded );
        int32_t treeNodeFlags = ImGuiTreeNodeFlags_OpenOnDoubleClick;

        if ( pBone->m_children.empty() )
        {
            treeNodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
        }

        if( currentBoneID == m_selectedBoneID )
        {
            treeNodeFlags |= ImGuiTreeNodeFlags_Selected;
        }

        InlineString boneLabel;
        boneLabel.sprintf( "%d. %s", pBone->m_boneIdx, currentBoneID.c_str() );

        if ( hasBody )
        {
            ImGuiX::ScopedFont const sf( ImGuiX::Font::MediumBold, Colors::LimeGreen );
            pBone->m_isExpanded = ImGui::TreeNodeEx( boneLabel.c_str(), treeNodeFlags );
        }
        else
        {
            pBone->m_isExpanded = ImGui::TreeNodeEx( boneLabel.c_str(), treeNodeFlags );
        }

        // Handle bone selection
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByPopup ) )
        {
            if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) || ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
            {
                m_selectedBoneID = m_skeleton->GetBoneID( pBone->m_boneIdx );
            }
        }

        // Context Menu
        if ( ImGui::BeginPopupContextItem() )
        {
            if ( hasBody )
            {
                if ( ImGui::MenuItem( "Destroy Body" ) )
                {
                    DestroyBody( bodyIdx );
                }

                if ( ImGui::MenuItem( "Destroy All Child Bodies" ) )
                {
                    DestroyChildBodies( bodyIdx );
                }
            }
            else
            {
                if ( pRagdollDefinition->GetNumBodies() >= RagdollDefinition::s_maxNumBodies )
                {
                    ImGui::Text( "Max number of bodies reached" );
                }
                else
                {
                    if ( ImGui::MenuItem( "Create Body" ) )
                    {
                        CreateBody( currentBoneID );
                    }
                }
            }

            ImGui::EndPopup();
        }

        //-------------------------------------------------------------------------

        ImRect const nodeRect = ImRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );

        if ( pBone->m_isExpanded )
        {
            ImColor const TreeLineColor = ImGui::GetColorU32( ImGuiCol_TextDisabled );
            float const SmallOffsetX = 2;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
            verticalLineStart.x += SmallOffsetX;
            ImVec2 verticalLineEnd = verticalLineStart;

            for ( BoneInfo* pChild : pBone->m_children )
            {
                const float HorizontalTreeLineSize = 4.0f;
                const ImRect childRect = RenderSkeletonTree( pChild );
                const float midpoint = ( childRect.Min.y + childRect.Max.y ) / 2.0f;
                drawList->AddLine( ImVec2( verticalLineStart.x, midpoint ), ImVec2( verticalLineStart.x + HorizontalTreeLineSize, midpoint ), TreeLineColor );
                verticalLineEnd.y = midpoint;
            }

            drawList->AddLine( verticalLineStart, verticalLineEnd, TreeLineColor );
            ImGui::TreePop();
        }

        return nodeRect;
    }

    //-------------------------------------------------------------------------
    // Profiles
    //-------------------------------------------------------------------------

    void RagdollWorkspace::SetActiveProfile( StringID profileID )
    {
        m_activeProfileID = profileID;
        UpdateProfileWorkingCopy();

        if ( IsPreviewing() )
        {
            m_pRagdoll->SwitchProfile( m_activeProfileID );
        }
    }

    void RagdollWorkspace::UpdateProfileWorkingCopy()
    {
        if ( !m_activeProfileID.IsValid() )
        {
            return;
        }

        // Handle invalid profile IDs
        auto pRagdollDef = GetRagdollDefinition();
        auto pProfile = pRagdollDef->GetProfile( m_activeProfileID );
        if ( pProfile == nullptr )
        {
            m_activeProfileID.Clear();
            return;
        }

        // Copy profile
        m_workingProfileCopy = *pProfile;
    }

    String RagdollWorkspace::GetUniqueProfileName( String const& desiredName ) const
    {
        String uniqueName = desiredName;
        bool isNameUnique = false;
        int32_t suffixCounter = 0;

        auto pRagdollDefinition = GetRagdollDefinition();
        while ( !isNameUnique )
        {
            isNameUnique = true;

            // Check control parameters
            for ( auto const& profile : pRagdollDefinition->m_profiles )
            {
                if ( _stricmp( uniqueName.c_str(), profile.m_ID.c_str() ) == 0 )
                {
                    isNameUnique = false;
                    break;
                }
            }

            if ( !isNameUnique )
            {
                uniqueName.sprintf( "%s_%d", desiredName.c_str(), suffixCounter );
                suffixCounter++;
            }
        }

        //-------------------------------------------------------------------------

        return uniqueName;
    }

    StringID RagdollWorkspace::CreateProfile( String const newProfileName )
    {
        auto const uniqueNameID = GetUniqueProfileName( newProfileName.c_str() );

        ScopedRagdollSettingsModification const sdm( this );
        auto pRagdollDefinition = GetRagdollDefinition();
        auto& profile = pRagdollDefinition->m_profiles.emplace_back();
        profile.m_ID = StringID( uniqueNameID.c_str() );
        ResetProfile( profile );
        return profile.m_ID;
    }

    StringID RagdollWorkspace::DuplicateProfile( StringID originalProfileID, String duplicateProfileName )
    {
        auto const uniqueNameID = GetUniqueProfileName( duplicateProfileName.c_str() );

        auto pRagdollDefinition = GetRagdollDefinition();
        auto pOriginalProfile = pRagdollDefinition->GetProfile( originalProfileID );
        EE_ASSERT( pOriginalProfile != nullptr );

        //-------------------------------------------------------------------------

        ScopedRagdollSettingsModification const sdm( this );
        auto& profile = pRagdollDefinition->m_profiles.emplace_back();
        profile.m_ID = StringID( uniqueNameID.c_str() );
        profile.CopySettingsFrom( *pOriginalProfile );
        return profile.m_ID;
    }

    void RagdollWorkspace::ResetProfile( RagdollDefinition::Profile& profile ) const
    {
        auto pRagdollDefinition = GetRagdollDefinition();
        int32_t const numBodies = pRagdollDefinition->GetNumBodies();

        //-------------------------------------------------------------------------

        if ( !profile.m_ID.IsValid() )
        {
            profile.m_ID = StringID( GetUniqueProfileName( "Profile" ) );
        }

        //-------------------------------------------------------------------------

        profile.m_bodySettings.clear();
        profile.m_materialSettings.clear();
        profile.m_jointSettings.clear();

        //-------------------------------------------------------------------------

        profile.m_bodySettings.resize( numBodies );
        profile.m_materialSettings.resize( numBodies );

        if ( numBodies > 1 )
        {
            profile.m_jointSettings.resize( numBodies - 1 );
        }
    }

    void RagdollWorkspace::DestroyProfile( StringID profileID )
    {
        ScopedRagdollSettingsModification const sdm( this );

        auto pRagdollDefinition = GetRagdollDefinition();
        for ( auto profileIter = pRagdollDefinition->m_profiles.begin(); profileIter != pRagdollDefinition->m_profiles.end(); ++profileIter )
        {
            if ( profileIter->m_ID == profileID )
            {
                pRagdollDefinition->m_profiles.erase( profileIter );
                break;
            }
        }
    }

    void RagdollWorkspace::DrawProfileEditorWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass )
    {
        auto pRagdollDefinition = GetRagdollDefinition();

        ImGui::SetNextWindowClass( pWindowClass );
        if ( ImGui::Begin( m_profileEditorWindowName.c_str() ) )
        {
            DrawProfileManager();

            //-------------------------------------------------------------------------

            RagdollDefinition::Profile* pActiveProfile = nullptr;
            if ( pRagdollDefinition->IsValid() )
            {
                pActiveProfile = pRagdollDefinition->GetProfile( m_activeProfileID );
            }

            //-------------------------------------------------------------------------

            if ( pActiveProfile == nullptr )
            {
                ImGui::TextColored( Colors::Red.ToFloat4(), "Invalid Definition" );
                if ( m_solverSettingsGrid.GetEditedType() != nullptr )
                {
                    m_solverSettingsGrid.SetTypeToEdit( nullptr );
                }
            }
            else if ( pRagdollDefinition->GetNumBodies() == 0 )
            {
                ImGui::TextColored( Colors::Yellow.ToFloat4(), "No Bodies" );
                if ( m_solverSettingsGrid.GetEditedType() != nullptr )
                {
                    m_solverSettingsGrid.SetTypeToEdit( nullptr );
                }
            }
            else
            {
                if ( ImGui::BeginTabBar( "ProfileSetings" ) )
                {
                    if ( ImGui::BeginTabItem( "Body/Joint Settings" ) )
                    {
                        DrawBodyAndJointSettingsTable( context, pActiveProfile );
                        ImGui::EndTabItem();
                    }

                    //-------------------------------------------------------------------------

                    if ( ImGui::BeginTabItem( "Material Settings" ) )
                    {
                        DrawMaterialSettingsTable( context, pActiveProfile );
                        ImGui::EndTabItem();
                    }

                    //-------------------------------------------------------------------------

                    if ( ImGui::BeginTabItem( "Solver Settings" ) )
                    {
                        if ( m_solverSettingsGrid.GetEditedType() != pActiveProfile )
                        {
                            m_solverSettingsGrid.SetTypeToEdit( pActiveProfile );
                        }
                        m_solverSettingsGrid.DrawGrid();
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
        }
        ImGui::End();
    }

    void RagdollWorkspace::DrawProfileManager()
    {
        constexpr static float const iconButtonWidth = 24;
        constexpr static float const createButtonWidth = 60;
        float const spacing = ImGui::GetStyle().ItemSpacing.x;

        auto pRagdollDefinition = GetRagdollDefinition();
        if ( !pRagdollDefinition->IsValid() )
        {
            return;
        }

        // Profile Selector
        //-------------------------------------------------------------------------

        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ( createButtonWidth + spacing ) - ( 3 * ( iconButtonWidth + spacing ) ) );
        if ( ImGui::BeginCombo( "##ProfileSelector", m_activeProfileID.IsValid() ? m_activeProfileID.c_str() : "Invalid Profile ID" ) )
        {
            for ( auto const& profile : pRagdollDefinition->m_profiles )
            {
                bool const isSelected = ( profile.m_ID == m_activeProfileID );
                if ( ImGui::Selectable( profile.m_ID.c_str(), isSelected ) )
                {
                    SetActiveProfile( profile.m_ID );
                }

                if ( isSelected )
                {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }

        // Controls
        //-------------------------------------------------------------------------

        ImGui::BeginDisabled( IsPreviewing() );
        {
            ImGui::SameLine();
            if ( ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"New", ImVec2( createButtonWidth, 0 ) ) )
            {
                auto const newUniqueName = GetUniqueProfileName( "Profile" );
                Printf( m_profileNameBuffer, 256, newUniqueName.c_str() );
                m_activeOperation = Operation::CreateProfile;
            }

            //-------------------------------------------------------------------------

            ImGui::SameLine();
            if ( ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_CONTENT_COPY"##Duplicate", ImVec2( iconButtonWidth, 0 ) ) )
            {
                auto const newUniqueName = GetUniqueProfileName( m_activeProfileID.c_str() );
                Printf( m_profileNameBuffer, 256, newUniqueName.c_str() );
                m_activeOperation = Operation::DuplicateProfile;
            }

            //-------------------------------------------------------------------------

            ImGui::SameLine();
            if ( ImGuiX::ColoredButton( Colors::RoyalBlue, Colors::White, EE_ICON_RENAME_BOX"##Rename", ImVec2( iconButtonWidth, 0 ) ) )
            {
                Printf( m_profileNameBuffer, 256, m_activeProfileID.c_str() );
                m_activeOperation = Operation::RenameProfile;
            }
            ImGuiX::ItemTooltip( "Rename Current Profile" );

            //-------------------------------------------------------------------------

            ImGui::SameLine();
            ImGui::BeginDisabled( pRagdollDefinition->GetNumProfiles() == 1 );
            if ( ImGuiX::ColoredButton( Colors::MediumRed, Colors::White, EE_ICON_DELETE, ImVec2( iconButtonWidth, 0 ) ) )
            {
                if ( pRagdollDefinition->GetNumProfiles() > 1 )
                {
                    DestroyProfile( m_activeProfileID );
                    SetActiveProfile( pRagdollDefinition->GetDefaultProfile()->m_ID );
                }
            }
            ImGui::EndDisabled();
            ImGuiX::ItemTooltip( "Delete Current Profile" );
        }
        ImGui::EndDisabled();
    }

    void RagdollWorkspace::DrawBodyAndJointSettingsTable( UpdateContext const& context, RagdollDefinition::Profile* pProfile )
    {
        EE_ASSERT( pProfile != nullptr );
        auto pRagdollDefinition = GetRagdollDefinition();

        int32_t const numBodies = pRagdollDefinition->GetNumBodies();
        int32_t const numJoints = numBodies - 1;

        //-------------------------------------------------------------------------

        ImGuiX::ScopedFont sf( ImGuiX::Font::Tiny );
        if ( ImGui::BeginTable( "BST", 20, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2( 0, ImGui::GetContentRegionAvail().y ) ) )
        {
            ImGui::TableSetupColumn( "Body", ImGuiTableColumnFlags_WidthFixed, 120 );
            ImGui::TableSetupColumn( "Mass", ImGuiTableColumnFlags_WidthFixed, 40 );
            ImGui::TableSetupColumn( "CCD", ImGuiTableColumnFlags_WidthFixed, 20 );
            ImGui::TableSetupColumn( "Self Collision", ImGuiTableColumnFlags_WidthFixed, 20 );
            ImGui::TableSetupColumn( "Follow Pose", ImGuiTableColumnFlags_WidthFixed, 20 );
            
            ImGui::TableSetupColumn( "Int. Compliance", ImGuiTableColumnFlags_WidthFixed, 50 );
            ImGui::TableSetupColumn( "Ext. Compliance", ImGuiTableColumnFlags_WidthFixed, 50 );

            ImGui::TableSetupColumn( "Stiffness", ImGuiTableColumnFlags_WidthFixed, 50 );
            ImGui::TableSetupColumn( "Damping", ImGuiTableColumnFlags_WidthFixed, 50 );
            ImGui::TableSetupColumn( "Velocity", ImGuiTableColumnFlags_WidthFixed, 20 );

            ImGui::TableSetupColumn( "Twist", ImGuiTableColumnFlags_WidthFixed, 20 );
            ImGui::TableSetupColumn( "Twist Lmt Min", ImGuiTableColumnFlags_WidthFixed, 60 );
            ImGui::TableSetupColumn( "Twist Lmt Max", ImGuiTableColumnFlags_WidthFixed, 60 );
            ImGui::TableSetupColumn( "Twist C-Dist", ImGuiTableColumnFlags_WidthFixed, 60 );

            ImGui::TableSetupColumn( "Swing", ImGuiTableColumnFlags_WidthFixed, 20 );
            ImGui::TableSetupColumn( "Swing Lmt Y", ImGuiTableColumnFlags_WidthFixed, 60 );
            ImGui::TableSetupColumn( "Swing Lmt Z", ImGuiTableColumnFlags_WidthFixed, 60 );
            ImGui::TableSetupColumn( "Swing C-Dist", ImGuiTableColumnFlags_WidthFixed, 60 );
            ImGui::TableSetupColumn( "Tang. Stiffness", ImGuiTableColumnFlags_WidthFixed, 50 );
            ImGui::TableSetupColumn( "Tang. Damping", ImGuiTableColumnFlags_WidthFixed, 50 );

            ImGui::TableHeadersRow();

            //-------------------------------------------------------------------------

            for ( auto bodyIdx = 0; bodyIdx < numBodies; bodyIdx++ )
            {
                auto& cachedBodySettings = m_workingProfileCopy.m_bodySettings[bodyIdx];
                auto& realBodySettings = pProfile->m_bodySettings[bodyIdx];

                //-------------------------------------------------------------------------
                // Body Settings
                //-------------------------------------------------------------------------

                ImGui::PushID( &cachedBodySettings );
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text( "%d. %s", bodyIdx, pRagdollDefinition->m_bodies[bodyIdx].m_boneID.c_str());

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::InputFloat( "##Mass", &cachedBodySettings.m_mass, 0.0f, 0.0f, "%.2f" ) )
                {
                    cachedBodySettings.m_mass = Math::Clamp( cachedBodySettings.m_mass, 0.01f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_mass = cachedBodySettings.m_mass;
                }
                ImGuiX::ItemTooltip( "The mass of the body. Note that physx articulations take into account the masses of all bodies in the articulation!" );

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::Checkbox( "##CCD", &cachedBodySettings.m_enableCCD );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_enableCCD = cachedBodySettings.m_enableCCD;
                }
                ImGuiX::ItemTooltip( "Enable continuous collision detection for this body" );

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::Checkbox( "##SC", &cachedBodySettings.m_enableSelfCollision );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_enableSelfCollision = cachedBodySettings.m_enableSelfCollision;
                }
                ImGuiX::ItemTooltip( "Should this body collide with other bodies in the ragdoll" );

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );

                if ( ImGui::Checkbox( "##FP", &cachedBodySettings.m_followPose ) )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_followPose = cachedBodySettings.m_followPose;
                }

                ImGuiX::ItemTooltip( "The should this body be driven towards the target pose via linear/angular velocities!" );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        for ( int32_t i = 0; i < numBodies; i++ )
                        {
                            m_workingProfileCopy.m_bodySettings[i].m_followPose = cachedBodySettings.m_followPose;
                            pProfile->m_bodySettings[i].m_followPose = cachedBodySettings.m_followPose;
                        }
                    }
                    ImGui::EndPopup();
                }

                if ( bodyIdx == 0 )
                {
                    continue;
                }

                //-------------------------------------------------------------------------
                // Joint Settings
                //-------------------------------------------------------------------------

                int32_t const jointIdx = bodyIdx - 1;
                auto& cachedJointSettings = m_workingProfileCopy.m_jointSettings[jointIdx];
                auto& realJointSettings = pProfile->m_jointSettings[jointIdx];

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##IntCompliance", &cachedJointSettings.m_internalCompliance, Math::HugeEpsilon, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_internalCompliance = cachedJointSettings.m_internalCompliance;
                }
                ImGuiX::ItemTooltip( "How much resistance does this joint have to internal (joint drive) forces. 0 = max resistance, 1 = no resistance." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_internalCompliance;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_internalCompliance = value;
                            pProfile->m_jointSettings[i].m_internalCompliance = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##ExtCompliance", &cachedJointSettings.m_externalCompliance, Math::HugeEpsilon, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_externalCompliance = cachedJointSettings.m_externalCompliance;
                }
                ImGuiX::ItemTooltip( "How much resistance does this joint have to internal (gravity/contact) forces. 0 = max resistance, 1 = no resistance" );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_externalCompliance;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_externalCompliance = value;
                            pProfile->m_jointSettings[i].m_externalCompliance = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::InputFloat( "##Stiffness", &cachedJointSettings.m_stiffness, 0.0f, 0.0f, "%.1f" ) )
                {
                    cachedJointSettings.m_stiffness = Math::Clamp( cachedJointSettings.m_stiffness, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_stiffness = cachedJointSettings.m_stiffness;
                }
                ImGuiX::ItemTooltip( "The acceleration generated by the spring drive is proportional to this value and the angle between the drive target position and the current position" );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_stiffness;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_stiffness = value;
                            pProfile->m_jointSettings[i].m_stiffness = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::InputFloat( "##Damping", &cachedJointSettings.m_damping, 0.0f, 0.0f, "%.1f" ) )
                {
                    cachedJointSettings.m_damping = Math::Clamp( cachedJointSettings.m_damping, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_damping = cachedJointSettings.m_damping;
                }
                ImGuiX::ItemTooltip( "The acceleration generated by the spring drive is proportional to this value and the difference between the angular velocity of the joint and the target drive velocity." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_damping;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_damping = value;
                            pProfile->m_jointSettings[i].m_damping = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::Checkbox( "##Velocity", &cachedJointSettings.m_useVelocity );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_useVelocity = cachedJointSettings.m_useVelocity;
                }
                ImGuiX::ItemTooltip( "Use velocity to drive joint (in addition to joint target)." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        bool const value = cachedJointSettings.m_useVelocity;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_useVelocity = value;
                            pProfile->m_jointSettings[i].m_useVelocity = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::Checkbox( "##Twist", &cachedJointSettings.m_twistLimitEnabled );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_twistLimitEnabled = cachedJointSettings.m_twistLimitEnabled;
                }
                ImGuiX::ItemTooltip( "Enable Twist Limits." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        bool const value = cachedJointSettings.m_twistLimitEnabled;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_twistLimitEnabled = value;
                            pProfile->m_jointSettings[i].m_twistLimitEnabled = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##TwistLimitMin", &cachedJointSettings.m_twistLimitMin, -179, cachedJointSettings.m_twistLimitMax - 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    EE_ASSERT( cachedJointSettings.m_twistLimitMin < cachedJointSettings.m_twistLimitMax );
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_twistLimitMin = cachedJointSettings.m_twistLimitMin;
                }
                ImGuiX::ItemTooltip( "The lower limit value must be less than the upper limit if the limit is enabled." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_twistLimitMin;
                        float const maxTwistContactDistance = ( cachedJointSettings.m_twistLimitMax - value ) / 2;

                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            if ( value < m_workingProfileCopy.m_jointSettings[i].m_twistLimitMax )
                            {
                                m_workingProfileCopy.m_jointSettings[i].m_twistLimitMin = value;
                                if ( m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance >= maxTwistContactDistance )
                                {
                                    m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance = maxTwistContactDistance / 2;
                                }

                                pProfile->m_jointSettings[i].m_twistLimitMin = m_workingProfileCopy.m_jointSettings[i].m_twistLimitMin;
                                pProfile->m_jointSettings[i].m_twistLimitContactDistance = m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##TwistLimitMax", &cachedJointSettings.m_twistLimitMax, cachedJointSettings.m_twistLimitMin + 1, 179, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    EE_ASSERT( cachedJointSettings.m_twistLimitMin < cachedJointSettings.m_twistLimitMax );
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_twistLimitMax = cachedJointSettings.m_twistLimitMax;
                }
                ImGuiX::ItemTooltip( "The lower limit value must be less than the upper limit if the limit is enabled." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_twistLimitMax;
                        float const maxTwistContactDistance = ( value - cachedJointSettings.m_twistLimitMin ) / 2;

                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            if ( value > m_workingProfileCopy.m_jointSettings[i].m_twistLimitMin )
                            {
                                m_workingProfileCopy.m_jointSettings[i].m_twistLimitMax = value;
                                if ( m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance >= maxTwistContactDistance )
                                {
                                    m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance = maxTwistContactDistance / 2;
                                }

                                pProfile->m_jointSettings[i].m_twistLimitMax = m_workingProfileCopy.m_jointSettings[i].m_twistLimitMax;
                                pProfile->m_jointSettings[i].m_twistLimitContactDistance = m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                // The contact distance should be less than half the distance between the upper and lower limits.
                float const maxTwistContactDistance = ( cachedJointSettings.m_twistLimitMax - cachedJointSettings.m_twistLimitMin ) / 2;
                if ( cachedJointSettings.m_twistLimitContactDistance >= maxTwistContactDistance )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    cachedJointSettings.m_twistLimitContactDistance = maxTwistContactDistance / 2;
                    realJointSettings.m_twistLimitContactDistance = cachedJointSettings.m_twistLimitContactDistance;
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##TwistContactDistance", &cachedJointSettings.m_twistLimitContactDistance, 0, maxTwistContactDistance - Math::HugeEpsilon, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_twistLimitContactDistance = cachedJointSettings.m_twistLimitContactDistance;
                }
                ImGuiX::ItemTooltip( "The contact distance should be less than half the distance between the upper and lower limits." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_twistLimitContactDistance;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_twistLimitContactDistance = value;
                            pProfile->m_jointSettings[i].m_twistLimitContactDistance = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::Checkbox( "##Swing", &cachedJointSettings.m_swingLimitEnabled );
                ImGuiX::ItemTooltip( "Enable Swing Limits." );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_swingLimitEnabled = cachedJointSettings.m_swingLimitEnabled;
                }

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );

                        bool const value = cachedJointSettings.m_swingLimitEnabled;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_swingLimitEnabled = value;
                            pProfile->m_jointSettings[i].m_swingLimitEnabled = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##SwingLimitY", &cachedJointSettings.m_swingLimitY, 1, 179, "%.1f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_swingLimitY = cachedJointSettings.m_swingLimitY;
                }
                ImGuiX::ItemTooltip( "Limit the allowed extent of rotation around the y-axis." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );

                        float const value = cachedJointSettings.m_swingLimitY;
                        float const maxSwingContactDistance = Math::Min( cachedJointSettings.m_swingLimitY, cachedJointSettings.m_swingLimitZ );

                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_swingLimitY = value;
                            if ( m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance >= maxSwingContactDistance )
                            {
                                m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance = maxSwingContactDistance / 2;
                            }

                            pProfile->m_jointSettings[i].m_swingLimitY = m_workingProfileCopy.m_jointSettings[i].m_swingLimitY;
                            pProfile->m_jointSettings[i].m_swingLimitContactDistance = m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##SwingLimitZ", &cachedJointSettings.m_swingLimitZ, 1, 179, "%.1f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_swingLimitZ = cachedJointSettings.m_swingLimitZ;
                }
                ImGuiX::ItemTooltip( "Limit the allowed extent of rotation around the z-axis." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_swingLimitZ;
                        float const maxSwingContactDistance = Math::Min( cachedJointSettings.m_swingLimitY, cachedJointSettings.m_swingLimitZ );

                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_swingLimitZ = value;
                            if ( m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance >= maxSwingContactDistance )
                            {
                                m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance = maxSwingContactDistance / 2;
                            }

                            pProfile->m_jointSettings[i].m_swingLimitZ = m_workingProfileCopy.m_jointSettings[i].m_swingLimitZ;
                            pProfile->m_jointSettings[i].m_swingLimitContactDistance = m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                // The contact distance should be less than either limit angle
                float const maxSwingContactDistance = Math::Min( cachedJointSettings.m_swingLimitY, cachedJointSettings.m_swingLimitZ );
                if ( cachedJointSettings.m_swingLimitContactDistance >= maxSwingContactDistance )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    cachedJointSettings.m_swingLimitContactDistance = maxSwingContactDistance - 1.0f;
                    realJointSettings.m_swingLimitContactDistance = cachedJointSettings.m_swingLimitContactDistance;
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##SwingContactDistance", &cachedJointSettings.m_swingLimitContactDistance, 0, maxSwingContactDistance - Math::HugeEpsilon, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_swingLimitContactDistance = cachedJointSettings.m_swingLimitContactDistance;
                }
                ImGuiX::ItemTooltip( "The contact distance should be less than either limit angle." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_swingLimitContactDistance;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_swingLimitContactDistance = value;
                            pProfile->m_jointSettings[i].m_swingLimitContactDistance = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::InputFloat( "##TangStiffness", &cachedJointSettings.m_tangentialStiffness, 0.0f, 0.0f, "%.1f" ) )
                {
                    cachedJointSettings.m_tangentialStiffness = Math::Clamp( cachedJointSettings.m_tangentialStiffness, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_tangentialStiffness = cachedJointSettings.m_tangentialStiffness;
                }
                ImGuiX::ItemTooltip( "The tangential stiffness for the swing limit cone." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_tangentialStiffness;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_tangentialStiffness = value;
                            pProfile->m_jointSettings[i].m_tangentialStiffness = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::InputFloat( "##TangDamping", &cachedJointSettings.m_tangentialDamping, 0.0f, 0.0f, "%.1f" ) )
                {
                    cachedJointSettings.m_tangentialDamping = Math::Clamp( cachedJointSettings.m_tangentialDamping, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realJointSettings.m_tangentialDamping = cachedJointSettings.m_tangentialDamping;
                }
                ImGuiX::ItemTooltip( "The tangential damping for the swing limit cone." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = cachedJointSettings.m_tangentialDamping;
                        for ( int32_t i = 0; i < numJoints; i++ )
                        {
                            m_workingProfileCopy.m_jointSettings[i].m_tangentialDamping = value;
                            pProfile->m_jointSettings[i].m_tangentialDamping = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }

            //-------------------------------------------------------------------------

            ImGui::EndTable();
        }
    }

    void RagdollWorkspace::DrawMaterialSettingsTable( UpdateContext const& context, RagdollDefinition::Profile* pProfile )
    {
        static char const* const comboOptions[4] = { "Average", "Min", "Multiply", "Max" };

        EE_ASSERT( pProfile != nullptr );
        auto pRagdollDefinition = GetRagdollDefinition();

        //-------------------------------------------------------------------------

        ImGuiX::ScopedFont sf( ImGuiX::Font::Small );
        if ( ImGui::BeginTable( "MaterialSettingsTable", 6, ImGuiTableFlags_BordersInnerV ) )
        {
            ImGui::TableSetupColumn( "Body", ImGuiTableColumnFlags_WidthFixed, 120 );
            ImGui::TableSetupColumn( "Static Friction", ImGuiTableColumnFlags_WidthFixed, 100 );
            ImGui::TableSetupColumn( "Dynamic Friction", ImGuiTableColumnFlags_WidthFixed, 100 );
            ImGui::TableSetupColumn( "Restitution", ImGuiTableColumnFlags_WidthFixed, 100 );
            ImGui::TableSetupColumn( "Friction Combine", ImGuiTableColumnFlags_WidthFixed, 100 );
            ImGui::TableSetupColumn( "Restitution Combine", ImGuiTableColumnFlags_WidthFixed, 100 );

            ImGui::TableHeadersRow();

            //-------------------------------------------------------------------------

            int32_t const numBodies = pRagdollDefinition->GetNumBodies();
            for ( auto bodyIdx = 0; bodyIdx < numBodies; bodyIdx++ )
            {
                auto& bodySettings = m_workingProfileCopy.m_materialSettings[bodyIdx];
                auto& realBodySettings = pProfile->m_materialSettings[bodyIdx];

                ImGui::PushID( &bodySettings );
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text( pRagdollDefinition->m_bodies[bodyIdx].m_boneID.c_str() );

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if ( ImGui::SliderFloat( "##StaticFriction", &bodySettings.m_staticFriction, 0, 10000, "%.3f", ImGuiSliderFlags_AlwaysClamp ) )
                {
                    bodySettings.m_staticFriction = Math::Clamp( bodySettings.m_staticFriction, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_staticFriction = bodySettings.m_staticFriction;
                }
                ImGuiX::ItemTooltip( "The coefficient of static friction." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = bodySettings.m_staticFriction;
                        for ( int32_t i = 0; i < numBodies; i++ )
                        {
                            m_workingProfileCopy.m_materialSettings[i].m_staticFriction = value;
                            pProfile->m_materialSettings[i].m_staticFriction = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                if( ImGui::SliderFloat( "##DynamicFriction", &bodySettings.m_dynamicFriction, 0, 10000, "%.3f", ImGuiSliderFlags_AlwaysClamp ) )
                {
                    bodySettings.m_dynamicFriction = Math::Clamp( bodySettings.m_dynamicFriction, 0.0f, 1000000.0f );
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_dynamicFriction = bodySettings.m_dynamicFriction;
                }
                ImGuiX::ItemTooltip( "The coefficient of dynamic friction." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = bodySettings.m_dynamicFriction;
                        for ( int32_t i = 0; i < numBodies; i++ )
                        {
                            m_workingProfileCopy.m_materialSettings[i].m_dynamicFriction = value;
                            pProfile->m_materialSettings[i].m_dynamicFriction = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##Restitution", &bodySettings.m_restitution, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp );
                if ( ImGui::IsItemDeactivatedAfterEdit() )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_restitution = bodySettings.m_restitution;
                }
                ImGuiX::ItemTooltip( "A coefficient of 0 makes the object bounce as little as possible, higher values up to 1.0 result in more bounce." );

                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Set To All" ) )
                    {
                        ScopedRagdollSettingsModification const sdm( this );
                        float const value = bodySettings.m_restitution;
                        for ( int32_t i = 0; i < numBodies; i++ )
                        {
                            m_workingProfileCopy.m_materialSettings[i].m_restitution = value;
                            pProfile->m_materialSettings[i].m_restitution = value;
                        }
                    }
                    ImGui::EndPopup();
                }

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                int32_t fc = (int32_t) bodySettings.m_frictionCombineMode;
                if( ImGui::Combo( "##FrictionCombine", &fc, comboOptions, 4 ) )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_frictionCombineMode = (PhysicsCombineMode) fc;
                    bodySettings.m_frictionCombineMode = (PhysicsCombineMode) fc;
                }
                ImGuiX::ItemTooltip( "Friction combine mode to set for this material" );

                //-------------------------------------------------------------------------

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1 );
                int32_t rc = (int32_t) bodySettings.m_restitutionCombineMode;
                if( ImGui::Combo( "##RestitutionCombine", &rc, comboOptions, 4 ) )
                {
                    ScopedRagdollSettingsModification const sdm( this );
                    realBodySettings.m_restitutionCombineMode = (PhysicsCombineMode) rc;
                    bodySettings.m_restitutionCombineMode = (PhysicsCombineMode) rc;
                }
                ImGuiX::ItemTooltip( "Restitution combine mode for this material" );

                ImGui::PopID();
            }

            //-------------------------------------------------------------------------

            ImGui::EndTable();
        }
    }

    //-------------------------------------------------------------------------
    // Preview
    //-------------------------------------------------------------------------

    void RagdollWorkspace::DrawPreviewControlsWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass )
    {
        auto pRagdollDefinition = GetRagdollDefinition();

        ImGui::SetNextWindowClass( pWindowClass );
        if ( ImGui::Begin( m_previewControlsWindowName.c_str() ) )
        {
            if ( IsPreviewing() )
            {
                if ( ImGuiX::ColoredButton( Colors::MediumRed, Colors::White, EE_ICON_STOP" Stop Preview", ImVec2( -1, 0 ) ) )
                {
                    StopPreview();
                }
            }
            else
            {
                ImGui::BeginDisabled( !pRagdollDefinition->IsValid() && m_pMeshComponent != nullptr );
                if ( ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLAY" Start Preview", ImVec2( -1, 0 ) ) )
                {
                    StartPreview( context );
                }
                ImGui::EndDisabled();
            }

            //-------------------------------------------------------------------------
            // General Settings
            //-------------------------------------------------------------------------

            ImGui::SetNextItemOpen( false, ImGuiCond_FirstUseEver );
            if ( ImGui::CollapsingHeader( "Start Transform" ) )
            {
                ImGui::Indent();

                ImGuiX::InputTransform( "StartTransform", m_previewStartTransform, -1.0f );

                if ( ImGui::Button( EE_ICON_RESTART" Reset Start Transform", ImVec2( -1, 0 ) ) )
                {
                    m_previewStartTransform = Transform::Identity;
                }

                ImGui::Unindent();
            }

            //-------------------------------------------------------------------------
            // Ragdoll
            //-------------------------------------------------------------------------

            ImGui::SetNextItemOpen( true, ImGuiCond_FirstUseEver );
            if ( ImGui::CollapsingHeader( "Ragdoll Settings" ) )
            {
                ImGui::Indent();

                if ( ImGui::Checkbox( "Enable Gravity", &m_enableGravity ) )
                {
                    if ( IsPreviewing() )
                    {
                        m_pRagdoll->SetGravityEnabled( m_enableGravity );
                    }
                }

                if ( ImGui::Checkbox( "Enable Pose Following", &m_enablePoseFollowing ) )
                {
                    if ( IsPreviewing() )
                    {
                        m_pRagdoll->SetPoseFollowingEnabled( m_enablePoseFollowing );
                    }
                }

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "Blend Weight:" );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##PhysicsWeight", &m_physicsBlendWeight, 0, 1, "%.3f", ImGuiSliderFlags_NoInput );

                ImGui::BeginDisabled( !IsPreviewing() );
                if ( ImGui::Button( EE_ICON_RESTART" Reset Ragdoll State", ImVec2( -1, 0 ) ) )
                {
                    m_pRagdoll->ResetState();
                    m_pMeshComponent->SetWorldTransform( m_previewStartTransform );
                    m_initializeRagdollPose = true;
                }
                ImGui::EndDisabled();

                ImGui::Unindent();
            }

            //-------------------------------------------------------------------------
            // Animation
            //-------------------------------------------------------------------------

            ImGui::SetNextItemOpen( true, ImGuiCond_FirstUseEver );
            if ( ImGui::CollapsingHeader( "Animation" ) )
            {
                ImGui::Indent();

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "Preview Anim:" );
                ImGui::SameLine();

                ResourcePath newPath;
                if ( m_resourceFilePicker.DrawPicker( m_previewAnimation, newPath ) )
                {
                    // If we need to change resource ID, switch IDs
                    ResourceID const selectedResourceID = newPath;
                    if ( selectedResourceID != m_previewAnimation.GetResourceID() )
                    {
                        if ( m_previewAnimation.IsSet() && m_previewAnimation.WasRequested() )
                        {
                            UnloadResource( &m_previewAnimation );
                        }

                        m_previewAnimation = selectedResourceID.IsValid() ? selectedResourceID : nullptr;

                        if ( m_previewAnimation.IsSet() )
                        {
                            LoadResource( &m_previewAnimation );
                        }
                    }
                }

                ImGui::Checkbox( "Enable Looping", &m_enableAnimationLooping );
                ImGui::Checkbox( "Apply Root Motion", &m_applyRootMotion );
                ImGui::Checkbox( "Auto Start", &m_autoPlayAnimation );

                //-------------------------------------------------------------------------

                bool const hasValidAndLoadedPreviewAnim = m_previewAnimation.IsSet() && m_previewAnimation.IsLoaded();
                ImGui::BeginDisabled( !IsPreviewing() || !hasValidAndLoadedPreviewAnim );

                ImVec2 const buttonSize( 30, 24 );
                if ( m_isPlayingAnimation )
                {
                    if ( ImGui::Button( EE_ICON_STOP"##StopAnim", buttonSize ) )
                    {
                        m_isPlayingAnimation = false;
                    }
                }
                else
                {
                    if ( ImGui::Button( EE_ICON_PLAY"##PlayAnim", buttonSize ) )
                    {
                        m_isPlayingAnimation = true;

                        if ( !m_enableAnimationLooping && m_animTime == 1.0f )
                        {
                            m_animTime = 0.0f;
                        }
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::SetNextItemWidth( -1 );
                float currentAnimTime = m_animTime.ToFloat();
                if ( ImGui::SliderFloat( "##AnimTime", &currentAnimTime, 0, 1, "%.3f", ImGuiSliderFlags_NoInput ) )
                {
                    m_isPlayingAnimation = false;
                    m_animTime = currentAnimTime;
                }

                ImGui::Unindent();
            }

            //-------------------------------------------------------------------------
            // Manipulation Controls
            //-------------------------------------------------------------------------

            ImGui::SetNextItemOpen( true, ImGuiCond_FirstUseEver );
            if ( ImGui::CollapsingHeader( "Manipulation" ) )
            {
                ImGui::Indent();

                constexpr float offset = 95;

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "Impulse Str:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##IS", &m_impulseStrength, 0.1f, 1000.0f );
                ImGuiX::ItemTooltip( "Applied impulse strength" );

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "CS Radius:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##CAR", &m_collisionActorRadius, 0.1f, 1.0f );
                ImGuiX::ItemTooltip( "Spawned Collision Sphere Radius" );

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "CS Mass:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##CAM", &m_collisionActorMass, 1.0f, 500.0f );
                ImGuiX::ItemTooltip( "Spawned Collision Sphere Mass" );

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "CS Velocity:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##CAV", &m_collisionActorInitialVelocity, 1.0f, 250.0f );
                ImGuiX::ItemTooltip( "Spawned Collision Sphere Velocity" );

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "CS Gravity:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::Checkbox( "##CAG", &m_collisionActorGravity );
                ImGuiX::ItemTooltip( "Does the Spawned Collision Sphere have gravity" );

                ImGui::AlignTextToFramePadding();
                ImGui::Text( "CS Lifetime:" );
                ImGui::SameLine( offset );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##CAL", &m_collisionActorLifetime, 1.0f, 20.0f );
                ImGuiX::ItemTooltip( "Spawned Collision Sphere Lifetime" );

                ImGui::Unindent();
            }

            //-------------------------------------------------------------------------
            // PVD
            //-------------------------------------------------------------------------

            ImGui::SetNextItemOpen( false, ImGuiCond_FirstUseEver );
            if ( ImGui::CollapsingHeader( "PhysX Visual Debugger" ) )
            {
                ImGui::Indent();

                ImGui::BeginDisabled( IsPreviewing() );
                ImGui::Checkbox( "Auto Connect to PVD", &m_autoConnectToPVD );
                ImGui::EndDisabled();

                ImGui::BeginDisabled( !IsPreviewing() );
                if ( m_pPhysicsSystem != nullptr && m_pPhysicsSystem->IsConnectedToPVD() )
                {
                    if ( ImGui::Button( "Disconnect From PVD", ImVec2( -1, 0 ) ) )
                    {
                        m_pPhysicsSystem->DisconnectFromPVD();
                    }
                }
                else
                {
                    if ( ImGui::Button( "Connect To PVD", ImVec2( -1, 0 ) ) )
                    {
                        m_pPhysicsSystem->ConnectToPVD();
                    }
                }
                ImGui::EndDisabled();

                ImGui::Unindent();
            }
        }
        ImGui::End();
    }

    void RagdollWorkspace::StartPreview( UpdateContext const& context )
    {
        auto pRagdollDefinition = GetRagdollDefinition();
        EE_ASSERT( m_pRagdoll == nullptr && pRagdollDefinition->IsValid() );
        EE_ASSERT( m_skeleton.IsLoaded() );

        // Skeleton is already loaded so the LoadResource will immediately set the loaded skeleton ptr
        LoadResource( &pRagdollDefinition->m_skeleton );
        pRagdollDefinition->CreateRuntimeData();

        //-------------------------------------------------------------------------

        m_pPhysicsSystem = context.GetSystem<PhysicsSystem>();
        if ( m_autoConnectToPVD )
        {
            m_pPhysicsSystem->ConnectToPVD();
        }

        auto pPhysicsWorldSystem = m_pWorld->GetWorldSystem<PhysicsWorldSystem>();
        m_pRagdoll = pPhysicsWorldSystem->GetScene()->CreateRagdoll( pRagdollDefinition, m_activeProfileID, 0 );
        m_pRagdoll->SetGravityEnabled( m_enableGravity );
        m_pRagdoll->SetPoseFollowingEnabled( m_enablePoseFollowing );

        m_initializeRagdollPose = true;

        //-------------------------------------------------------------------------

        if ( m_autoPlayAnimation )
        {
            m_animTime = 0.0f;
            m_isPlayingAnimation = true;
        }

        m_pPose = EE::New<Animation::Pose>( m_skeleton.GetPtr() );
        m_pFinalPose = EE::New<Animation::Pose>( m_skeleton.GetPtr() );

        m_pMeshComponent->SetWorldTransform( m_previewStartTransform );
    }

    void RagdollWorkspace::StopPreview()
    {
        EE_ASSERT( m_pRagdoll != nullptr );

        //-------------------------------------------------------------------------

        EE_ASSERT( m_pPhysicsSystem != nullptr );
        if ( m_pPhysicsSystem->IsConnectedToPVD() )
        {
            m_pPhysicsSystem->DisconnectFromPVD();
        }
        m_pPhysicsSystem = nullptr;

        //-------------------------------------------------------------------------

        DestroySpawnedCollisionActors();

        //-------------------------------------------------------------------------

        m_pMeshComponent->ResetPose();
        m_pMeshComponent->FinalizePose();
        m_pMeshComponent->SetWorldTransform( Transform::Identity );

        m_pRagdoll->RemoveFromScene();
        EE::Delete( m_pRagdoll );

        auto pRagdollDefinition = GetRagdollDefinition();
        if ( pRagdollDefinition->m_skeleton.WasRequested() )
        {
            UnloadResource( &pRagdollDefinition->m_skeleton );
        }

        //-------------------------------------------------------------------------

        EE::Delete( m_pPose );
        EE::Delete( m_pFinalPose );
    }

    void RagdollWorkspace::SpawnCollisionActor( Vector const& startPos, Vector const& initialVelocity )
    {
        physx::PxScene* pPhysicsScene = m_pWorld->GetWorldSystem<PhysicsWorldSystem>()->GetPxScene();
        physx::PxPhysics* pPhysics = &pPhysicsScene->getPhysics();

        physx::PxMaterial* materials[] =
        {
            pPhysics->createMaterial( 0.5f, 0.5f, 0.5f )
        };

        auto pPhysicsShape = pPhysics->createShape( physx::PxSphereGeometry( m_collisionActorRadius ), materials, 1, true, physx::PxShapeFlag::eVISUALIZATION | physx::PxShapeFlag::eSIMULATION_SHAPE );
        pPhysicsShape->setSimulationFilterData( physx::PxFilterData( 0xFFFFFFFF, 0, 0, 0 ) );
        pPhysicsShape->setQueryFilterData( physx::PxFilterData( 0xFFFFFFFF, 0, 0, 0 ) );

        //-------------------------------------------------------------------------

        auto pPhysicsActor = pPhysics->createRigidDynamic( ToPx( Transform( Quaternion::Identity, startPos ) ) );
        pPhysicsActor->attachShape( *pPhysicsShape );
        pPhysicsShape->release();

        pPhysicsActor->setActorFlag( physx::PxActorFlag::eDISABLE_GRAVITY, !m_collisionActorGravity );
        physx::PxRigidBodyExt::setMassAndUpdateInertia( *pPhysicsActor, m_collisionActorMass );

        pPhysicsScene->lockWrite();
        pPhysicsScene->addActor( *pPhysicsActor );
        pPhysicsActor->setLinearVelocity( ToPx( initialVelocity ) );
        pPhysicsScene->unlockWrite();

        //-------------------------------------------------------------------------

        CollisionActor CA;
        CA.m_pActor = pPhysicsActor;
        CA.m_radius = m_collisionActorRadius;
        CA.m_TTL = m_collisionActorLifetime;

        m_spawnedCollisionActors.emplace_back( CA );
    }

    void RagdollWorkspace::UpdateSpawnedCollisionActors( Drawing::DrawContext& drawingContext, Seconds deltaTime )
    {
        physx::PxScene* pPhysicsScene = m_pWorld->GetWorldSystem<PhysicsWorldSystem>()->GetPxScene();
        pPhysicsScene->lockWrite();
        for ( int32_t i = int32_t( m_spawnedCollisionActors.size() ) - 1; i >= 0; i-- )
        {
            m_spawnedCollisionActors[i].m_TTL -= deltaTime.ToFloat();

            // Remove actor once lifetime exceeded
            if ( m_spawnedCollisionActors[i].m_TTL <= 0.0f )
            {
                pPhysicsScene->removeActor( *m_spawnedCollisionActors[i].m_pActor );
                m_spawnedCollisionActors[i].m_pActor->release();
                m_spawnedCollisionActors.erase( m_spawnedCollisionActors.begin() + i );
            }
            else // Draw actor
            {
                Transform actorTransform = FromPx( m_spawnedCollisionActors[i].m_pActor->getGlobalPose());
                drawingContext.DrawSphere( actorTransform, m_spawnedCollisionActors[i].m_radius, Colors::Red, 3.0f );
            }
        }
        pPhysicsScene->unlockWrite();
    }

    void RagdollWorkspace::DestroySpawnedCollisionActors()
    {
        physx::PxScene* pPhysicsScene = m_pWorld->GetWorldSystem<PhysicsWorldSystem>()->GetPxScene();
        pPhysicsScene->lockWrite();
        for ( auto& CA : m_spawnedCollisionActors )
        {
            pPhysicsScene->removeActor( *CA.m_pActor );
            CA.m_pActor->release();
        }
        pPhysicsScene->unlockWrite();

        m_spawnedCollisionActors.clear();
    }
}