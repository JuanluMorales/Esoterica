#include "PlayerGraphController_Ability.h"

//-------------------------------------------------------------------------

namespace EE::Player
{
    AbilityGraphController::AbilityGraphController( Animation::GraphInstance* pGraphInstance, Render::SkeletalMeshComponent* pMeshComponent )
        : Animation::SubGraphController( pGraphInstance, pMeshComponent )
    {
        m_abilityID.TryBind( this );
        m_facingParam.TryBind( this );
        m_headingParam.TryBind( this );
        m_speedParam.TryBind( this );
    }

    void AbilityGraphController::StartJump()
    {
        static StringID const jumpID( "Jump" );
        m_abilityID.Set( this, jumpID );
    }

    void AbilityGraphController::StartDash()
    {
        static StringID const dashID( "Dash" );
        m_abilityID.Set( this, dashID );
    }

    void AbilityGraphController::StartSlide()
    {
        static StringID const dashID( "Slide" );
        m_abilityID.Set( this, dashID );
    }

    void AbilityGraphController::SetDesiredMovement( Seconds const deltaTime, Vector const& headingVelocityWS, Vector const& facingDirectionWS )
    {
        Vector const characterSpaceHeading = ConvertWorldSpaceVectorToCharacterSpace( headingVelocityWS );
        float const speed = characterSpaceHeading.GetLength3();

        m_headingParam.Set( this, characterSpaceHeading );
        m_speedParam.Set( this, speed );

        //-------------------------------------------------------------------------

        if ( facingDirectionWS.IsZero3() )
        {
            m_facingParam.Set( this, Vector::WorldForward );
        }
        else
        {
            EE_ASSERT( facingDirectionWS.IsNormalized3() );
            Vector const characterSpaceFacing = ConvertWorldSpaceVectorToCharacterSpace( facingDirectionWS ).GetNormalized2();
            m_facingParam.Set( this, characterSpaceFacing );
        }
    }
}