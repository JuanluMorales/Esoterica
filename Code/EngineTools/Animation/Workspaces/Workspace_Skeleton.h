#pragma once

#include "EngineTools/Core/Workspace.h"
#include "EngineTools/Core/Helpers/SkeletonHelpers.h"
#include "Engine/Animation/AnimationSkeleton.h"
#include "System/Imgui/ImguiX.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    class SkeletalMeshComponent;
}

namespace EE::Animation
{
    class SkeletonWorkspace final : public TWorkspace<Skeleton>
    {
        enum class OperationType
        {
            None,
            RenameMask,
            DeleteMask
        };

    public:

        using TWorkspace::TWorkspace;
        virtual ~SkeletonWorkspace();

    private:

        virtual bool HasTitlebarIcon() const override { return true; }
        virtual char const* GetTitlebarIcon() const override { EE_ASSERT( HasTitlebarIcon() ); return EE_ICON_SKULL; }
        virtual void Initialize( UpdateContext const& context ) override;
        virtual void Shutdown( UpdateContext const& context ) override;
        virtual void InitializeDockingLayout( ImGuiID dockspaceID ) const override;
        virtual void Update( UpdateContext const& context, ImGuiWindowClass* pWindowClass, bool isFocused ) override;

        virtual void PostUndoRedo( UndoStack::Operation operation, IUndoableAction const* pAction ) override;
        virtual void OnHotReloadStarted( bool descriptorNeedsReload, TInlineVector<Resource::ResourcePtr*, 10> const& resourcesToBeReloaded ) override;
        virtual void OnHotReloadComplete() override;

        void CreatePreviewEntity();
        void DrawDialogs( UpdateContext const& context );

        // Skeleton
        //-------------------------------------------------------------------------

        void CreateSkeletonTree();
        void DestroySkeletonTree();

        void DrawSkeletonWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass );
        void DrawBoneInfoWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass );
        void DrawBoneRow( BoneInfo* pBone );

        void DrawSkeletonPreview();

        // Bone Masks
        //-------------------------------------------------------------------------

        void ValidateDescriptorBoneMaskDefinitions();

        bool IsEditingBoneMask() const { return m_editedMaskIdx != InvalidIndex; }
        void StartEditingMask( StringID maskID );
        void StopEditingMask();

        void CreateBoneMask();
        void DestroyBoneMask( StringID maskID );
        void RenameBoneMask( StringID oldID, StringID newID );

        void SetWeight( int32_t boneIdx, float weight );
        void SetAllChildWeights( int32_t parentBoneIdx, float weight, bool bIncludeParent = false );
        void UpdatePreviewBoneMask();

        void ReflectWeightsToEditedBoneMask();

        void DrawBoneMaskWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass );
        void DrawBoneMaskPreview();

        void GenerateUniqueBoneMaskName( int32_t boneMaskIdx );

    private:

        String                          m_skeletonWindowName;
        String                          m_boneInfoWindowName;
        String                          m_boneMasksWindowName;

        Entity*                         m_pPreviewEntity = nullptr;

        // Skeleton View
        BoneInfo*                       m_pSkeletonTreeRoot = nullptr;
        StringID                        m_selectedBoneID;

        // Bone Mask View
        int32_t                         m_editedMaskIdx = InvalidIndex;
        TVector<float>                  m_editedBoneWeights;
        BoneMask                        m_previewBoneMask;
        OperationType                   m_activeOperation = OperationType::None;
        StringID                        m_operationID;
        char                            m_renameBuffer[255] = { 0 };
    };
}