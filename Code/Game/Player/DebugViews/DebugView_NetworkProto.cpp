#include "DebugView_NetworkProto.h"
#include "Engine/Animation/Components/Component_AnimationGraph.h"
#include "Engine/Animation/TaskSystem/Animation_TaskSystem.h"
#include "Engine/Animation/DebugViews/DebugView_Animation.h"
#include "Engine/Entity/EntityWorld.h"
#include "Engine/Entity/EntitySystem.h"
#include "Engine/Entity/EntityWorldUpdateContext.h"
#include "Engine/Player/Systems/WorldSystem_PlayerManager.h"
#include "Engine/UpdateContext.h"
#include "System/Imgui/ImguiX.h"
#include "System/Math/MathStringHelpers.h"
#include "System/Resource/ResourceSystem.h"
#include "System/ThirdParty/implot/implot.h"

//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::Player
{
    void NetworkProtoDebugView::Initialize( SystemRegistry const& systemRegistry, EntityWorld const* pWorld )
    {
        m_pWorld = pWorld;
        m_pPlayerManager = pWorld->GetWorldSystem<PlayerManager>();
    }

    void NetworkProtoDebugView::Shutdown()
    {
        ResetRecordingData();
        m_pPlayerManager = nullptr;
        m_pWorld = nullptr;
    }

    void NetworkProtoDebugView::BeginHotReload( TVector<Resource::ResourceRequesterID> const& usersToReload, TVector<ResourceID> const& resourcesToBeReloaded )
    {
        if ( VectorContains( usersToReload, Resource::ResourceRequesterID( m_pPlayerGraphComponent->GetEntityID().m_value ) ) )
        {
            ResetRecordingData();
        }

        if ( VectorContains( resourcesToBeReloaded, m_graphRecorder.m_graphID ) )
        {
            ResetRecordingData();
        }
    }

    void NetworkProtoDebugView::DrawWindows( EntityWorldUpdateContext const& context, ImGuiWindowClass* pWindowClass )
    {
        EE_ASSERT( m_pWorld != nullptr );

        PlayerController* pPlayerController = nullptr;
        if ( m_isActionDebugWindowOpen || m_isCharacterControllerDebugWindowOpen )
        {
            if ( m_pPlayerManager->HasPlayer() )
            {
                auto pPlayerEntity = m_pWorld->GetPersistentMap()->FindEntity( m_pPlayerManager->GetPlayerEntityID() );
                for ( auto pComponent : pPlayerEntity->GetComponents() )
                {
                    m_pPlayerGraphComponent = TryCast<Animation::GraphComponent>( pComponent );
                    if ( m_pPlayerGraphComponent != nullptr )
                    {
                        break;
                    }
                }
            }
        }

        if ( m_pPlayerGraphComponent == nullptr )
        {
            ResetRecordingData();
            return;
        }

        //-------------------------------------------------------------------------

        if ( pWindowClass != nullptr ) ImGui::SetNextWindowClass( pWindowClass );
        if ( ImGui::Begin( "Network Proto" ) )
        {
            if ( m_isRecording )
            {
                if ( ImGuiX::IconButton( EE_ICON_STOP, " Stop Recording", ImGuiX::ImColors::White ) )
                {
                    m_pPlayerGraphComponent->GetDebugGraphInstance()->StopRecording();

                    //-------------------------------------------------------------------------

                    m_pActualInstance = EE::New<Animation::GraphInstance>( m_pPlayerGraphComponent->GetDebugGraphInstance()->GetGraphVariation(), 1 );
                    m_pReplicatedInstance = EE::New<Animation::GraphInstance>( m_pPlayerGraphComponent->GetDebugGraphInstance()->GetGraphVariation(), 2 );
                    m_pTaskSystem = EE::New<Animation::TaskSystem>( m_pPlayerGraphComponent->GetSkeleton() );
                    m_pGeneratedPose = EE::New<Animation::Pose>( m_pPlayerGraphComponent->GetSkeleton() );

                    ProcessRecording();
                    m_updateFrameIdx = 0;
                    m_isRecording = false;

                    GenerateTaskSystemPose();
                }
            }
            else
            {
                if ( ImGuiX::IconButton( EE_ICON_RECORD, " Start Recording", ImGuiX::ImColors::Red ) )
                {
                    ResetRecordingData();
                    m_pPlayerGraphComponent->GetDebugGraphInstance()->StartRecording( &m_graphRecorder );
                    m_isRecording = true;
                }
            }

            //-------------------------------------------------------------------------

            if ( m_graphRecorder.HasRecordedData() )
            {
                // Timeline
                ImGui::AlignTextToFramePadding();
                ImGui::Text( "Recorded Frame: " );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - 200 );
                if ( ImGui::SliderInt( "##timeline", &m_updateFrameIdx, 0, m_graphRecorder.GetNumRecordedFrames() - 1 ) )
                {
                    GenerateTaskSystemPose();
                }

                // Join in progress
                ImGui::BeginDisabled( m_updateFrameIdx < 0 );
                ImGui::SameLine();
                if ( ImGui::Button( "Fake Join In Progress##SIMJIP", ImVec2( 200, 0 ) ) )
                {
                    ProcessRecording( m_updateFrameIdx );
                }
                ImGui::EndDisabled();

                // Size graph
                //-------------------------------------------------------------------------

                if ( !m_isRecording )
                {
                    if ( ImPlot::BeginPlot( "Recorded Task Data", ImVec2( -1, 200 ), ImPlotFlags_NoMenus | ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend | ImPlotFlags_NoBoxSelect ) )
                    {
                        ImPlot::SetupAxes( "Time", "Size", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel );
                        ImPlot::PlotBars( "Vertical", m_serializedTaskSizes.data(), m_graphRecorder.GetNumRecordedFrames(), 1.0f );
                        double x = (double) m_updateFrameIdx;
                        if ( ImPlot::DragLineX( 0, &x, ImVec4( 1, 0, 0, 1 ), 2, 0 ) )
                        {
                            m_updateFrameIdx = Math::Clamp( (int32_t) x, 0, m_graphRecorder.GetNumRecordedFrames() - 1 );
                            GenerateTaskSystemPose();
                        }
                        ImPlot::EndPlot();
                    }
                }

                // Draw frame data info
                //-------------------------------------------------------------------------

                if ( m_updateFrameIdx != InvalidIndex )
                {
                    auto const& frameData = m_graphRecorder.m_recordedData[m_updateFrameIdx];

                    InlineString str( InlineString::CtorSprintf(), "Frame: %d", m_updateFrameIdx );
                    ImGuiX::TextSeparator( str.c_str() );

                    //-------------------------------------------------------------------------

                    ImGui::Text( "Sync Range: ( %d, %.2f%% ) -> (%d, %.2f%%)", frameData.m_updateRange.m_startTime.m_eventIdx, frameData.m_updateRange.m_startTime.m_percentageThrough.ToFloat(), frameData.m_updateRange.m_endTime.m_eventIdx, frameData.m_updateRange.m_endTime.m_percentageThrough.ToFloat() );

                    ImGui::Text( "Serialized Task Size: %d bytes", frameData.m_serializedTaskData.size() );

                    if ( ImGui::BeginTable( "RecData", 2, ImGuiTableFlags_BordersInner ) )
                    {
                        ImGui::TableSetupColumn( "Parameters", ImGuiTableColumnFlags_WidthStretch, 0.5f );
                        ImGui::TableSetupColumn( "Tasks", ImGuiTableColumnFlags_WidthStretch, 0.5f );
                        ImGui::TableHeadersRow();

                        // Draw Control Parameters
                        //-------------------------------------------------------------------------

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        if ( ImGui::BeginTable( "Params", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable ) )
                        {
                            ImGui::TableSetupColumn( "Label", ImGuiTableColumnFlags_WidthFixed, 300.0f );
                            ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch );

                            int32_t const numParameters = m_pActualInstance->GetNumControlParameters();
                            for ( int32_t i = 0; i < numParameters; i++ )
                            {
                                ImGui::TableNextRow();

                                ImGui::TableNextColumn();
                                ImGui::Text( m_pActualInstance->GetControlParameterID( (int16_t) i ).c_str() );

                                ImGui::TableNextColumn();
                                switch ( m_pActualInstance->GetControlParameterType( (int16_t) i ) )
                                {
                                    case Animation::GraphValueType::Bool:
                                    {
                                        ImGui::Text( frameData.m_parameterData[i].m_bool ? "True" : "False" );
                                    }
                                    break;

                                    case Animation::GraphValueType::ID:
                                    {
                                        if ( frameData.m_parameterData[i].m_ID.IsValid() )
                                        {
                                            ImGui::Text( frameData.m_parameterData[i].m_ID.c_str() );
                                        }
                                        else
                                        {
                                            ImGui::Text( "" );
                                        }
                                    }
                                    break;

                                    case Animation::GraphValueType::Int:
                                    {
                                        ImGui::Text( "%d", frameData.m_parameterData[i].m_int );
                                    }
                                    break;

                                    case Animation::GraphValueType::Float:
                                    {
                                        ImGui::Text( "%f", frameData.m_parameterData[i].m_float );
                                    }
                                    break;

                                    case Animation::GraphValueType::Vector:
                                    {
                                        ImGui::Text( Math::ToString( frameData.m_parameterData[i].m_vector ).c_str() );
                                    }
                                    break;

                                    case Animation::GraphValueType::Target:
                                    {
                                        InlineString stringValue;
                                        Animation::Target const value = frameData.m_parameterData[i].m_target;
                                        if ( !value.IsTargetSet() )
                                        {
                                            stringValue = "Unset";
                                        }
                                        else if ( value.IsBoneTarget() )
                                        {
                                            stringValue.sprintf( "Bone: %s", value.GetBoneID().c_str() );
                                        }
                                        else
                                        {
                                            stringValue = "TODO";
                                        }
                                        ImGui::Text( stringValue.c_str() );
                                    }
                                    break;

                                    default:
                                    break;
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Draw task list
                        //-------------------------------------------------------------------------

                        ImGui::TableNextColumn();
                        Animation::AnimationDebugView::DrawGraphActiveTasksDebugView( m_pTaskSystem );

                        ImGui::EndTable();
                    }
                }
            }

            //-------------------------------------------------------------------------

            if ( m_updateFrameIdx != InvalidIndex )
            {
                int32_t const nextFrameIdx = ( m_updateFrameIdx < m_graphRecorder.GetNumRecordedFrames() - 1 ) ? m_updateFrameIdx + 1 : m_updateFrameIdx;
                auto const& nextRecordedFrameData = m_graphRecorder.m_recordedData[nextFrameIdx];

                auto drawContext = context.GetDrawingContext();
                m_actualPoses[m_updateFrameIdx].DrawDebug( drawContext, nextRecordedFrameData.m_characterWorldTransform, Colors::LimeGreen, 4.0f );
                m_replicatedPoses[m_updateFrameIdx].DrawDebug( drawContext, nextRecordedFrameData.m_characterWorldTransform, Colors::HotPink, 4.0f );

                if ( m_pGeneratedPose != nullptr )
                {
                    m_pGeneratedPose->DrawDebug( drawContext, nextRecordedFrameData.m_characterWorldTransform, Colors::Orange, 4.0f );
                }
            }
        }
        ImGui::End();
    }

    void NetworkProtoDebugView::ResetRecordingData()
    {
        m_graphRecorder.Reset();
        m_isRecording = false;
        m_updateFrameIdx = InvalidIndex;
        EE::Delete( m_pActualInstance );
        EE::Delete( m_pReplicatedInstance );
        EE::Delete( m_pTaskSystem );
        EE::Delete( m_pGeneratedPose );
        m_actualPoses.clear();
        m_replicatedPoses.clear();
        m_serializedTaskSizes.clear();
    }

    void NetworkProtoDebugView::ProcessRecording( int32_t simulatedJoinInProgressFrame )
    {
        m_actualPoses.clear();
        m_replicatedPoses.clear();

        // Serialized tasks
        //-------------------------------------------------------------------------

        m_minSerializedTaskDataSize = FLT_MAX;
        m_maxSerializedTaskDataSize = -FLT_MAX;

        for ( auto const& frameData : m_graphRecorder.m_recordedData )
        {
            float const size = (float) frameData.m_serializedTaskData.size();
            m_minSerializedTaskDataSize = Math::Min( m_minSerializedTaskDataSize, size );
            m_maxSerializedTaskDataSize = Math::Max( m_maxSerializedTaskDataSize, size );
            m_serializedTaskSizes.emplace_back( size );
        }

        // Actual recording
        //-------------------------------------------------------------------------

        m_graphRecorder.m_initialState.PrepareForReading();
        m_pActualInstance->SetToRecordedState( m_graphRecorder.m_initialState );

        for ( auto i = 0; i < m_graphRecorder.GetNumRecordedFrames(); i++ )
        {
            auto const& frameData = m_graphRecorder.m_recordedData[i];
            int32_t const nextFrameIdx = ( i < m_graphRecorder.GetNumRecordedFrames() - 1 ) ? i + 1 : i;
            auto const& nextRecordedFrameData = m_graphRecorder.m_recordedData[nextFrameIdx];

            // Set parameters
            m_pActualInstance->SetRecordedFrameUpdateData( frameData );
            m_pActualInstance->EvaluateGraph( frameData.m_deltaTime, frameData.m_characterWorldTransform, nullptr, false );
            m_pActualInstance->ExecutePrePhysicsPoseTasks( nextRecordedFrameData.m_characterWorldTransform );
            m_pActualInstance->ExecutePostPhysicsPoseTasks();

            auto& pose = m_actualPoses.emplace_back( Animation::Pose( m_pActualInstance->GetPose()->GetSkeleton() ) );
            pose.CopyFrom( *m_pActualInstance->GetPose() );
        }

        // Replicated Recording
        //-------------------------------------------------------------------------

        bool const isSimulatingJoinInProgress = simulatedJoinInProgressFrame > 0;
        int32_t startFrameIdx = 0;
        if ( isSimulatingJoinInProgress )
        {
            EE_ASSERT( simulatedJoinInProgressFrame < ( m_graphRecorder.GetNumRecordedFrames() - 1 ) );

            for ( int32_t i = 0; i < simulatedJoinInProgressFrame; i++ )
            {
                m_replicatedPoses.emplace_back( Animation::Pose( m_pReplicatedInstance->GetPose()->GetSkeleton(), Animation::Pose::Type::ReferencePose ) );
            }

            startFrameIdx = simulatedJoinInProgressFrame;
        }

        // Set graph parameters before reset
        auto const& startFrameData = m_graphRecorder.m_recordedData[startFrameIdx];
        m_pReplicatedInstance->SetRecordedFrameUpdateData( startFrameData );
        m_pReplicatedInstance->ResetGraphState( startFrameData.m_updateRange.m_startTime );

        // Evaluate subsequent frames
        for ( auto i = startFrameIdx; i < m_graphRecorder.GetNumRecordedFrames(); i++ )
        {
            auto const& frameData = m_graphRecorder.m_recordedData[i];
            int32_t const nextFrameIdx = ( i < m_graphRecorder.GetNumRecordedFrames() - 1 ) ? i + 1 : i;
            auto const& nextRecordedFrameData = m_graphRecorder.m_recordedData[nextFrameIdx];

            // Set parameters
            m_pReplicatedInstance->SetRecordedFrameUpdateData( frameData );
            m_pReplicatedInstance->EvaluateGraph( frameData.m_deltaTime, frameData.m_characterWorldTransform, nullptr );
            m_pReplicatedInstance->ExecutePrePhysicsPoseTasks( nextRecordedFrameData.m_characterWorldTransform );
            m_pReplicatedInstance->ExecutePostPhysicsPoseTasks();

            auto& pose = m_replicatedPoses.emplace_back( Animation::Pose( m_pReplicatedInstance->GetPose()->GetSkeleton() ) );
            pose.CopyFrom( *m_pReplicatedInstance->GetPose() );
        }
    }

    void NetworkProtoDebugView::GenerateTaskSystemPose()
    {
        EE_ASSERT( m_pTaskSystem != nullptr && m_pGeneratedPose != nullptr );
        EE_ASSERT( m_graphRecorder.HasRecordedData() && !m_isRecording );
        EE_ASSERT( m_updateFrameIdx != InvalidIndex );

        auto const& frameData = m_graphRecorder.m_recordedData[m_updateFrameIdx];

        TInlineVector<Animation::ResourceLUT const*, 10> LUTs;
        m_pPlayerGraphComponent->GetDebugGraphInstance()->GetResourceLookupTables( LUTs );

        m_pTaskSystem->Reset();
        m_pTaskSystem->DeserializeTasks( LUTs, frameData.m_serializedTaskData );
        m_pTaskSystem->UpdatePrePhysics( frameData.m_deltaTime, frameData.m_characterWorldTransform, frameData.m_characterWorldTransform.GetInverse() );
        m_pTaskSystem->UpdatePostPhysics();
        m_pGeneratedPose->CopyFrom( m_pTaskSystem->GetPose() );
    }
}
#endif