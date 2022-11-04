#pragma once

#include "System/_Module/API.h"
#include "ImguiFont.h"
#include "System/ThirdParty/imgui/imgui.h"
#include "System/ThirdParty/imgui/imgui_internal.h"
#include "System/Math/Transform.h"
#include "System/Types/Color.h"
#include "System/Types/String.h"
#include "System/Types/BitFlags.h"

//-------------------------------------------------------------------------
// ImGui Extensions
//-------------------------------------------------------------------------
// This is the primary integration of DearImgui in Esoterica.
//
// * Provides the necessary imgui state updates through the frame start/end functions
// * Provides helpers for common operations
//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::ImGuiX
{
    //-------------------------------------------------------------------------
    // Color Helpers
    //-------------------------------------------------------------------------

    EE_FORCE_INLINE ImColor ConvertColor( Color const& color )
    {
        return IM_COL32( color.m_byteColor.m_r, color.m_byteColor.m_g, color.m_byteColor.m_b, color.m_byteColor.m_a );
    }

    EE_FORCE_INLINE Color ConvertColor( ImColor const& color )
    {
        return Color( uint8_t( color.Value.x * 255 ), uint8_t( color.Value.y * 255 ), uint8_t( color.Value.z * 255 ), uint8_t( color.Value.w * 255 ) );
    }

    // Adjust the brightness of color by a multiplier
    EE_FORCE_INLINE ImColor AdjustColorBrightness( ImColor const& color, float multiplier )
    {
        Float4 tmpColor = (ImVec4) color;
        float const alpha = tmpColor.m_w;
        tmpColor *= multiplier;
        tmpColor.m_w = alpha;
        return ImColor( tmpColor );
    }

    // Adjust the brightness of color by a multiplier
    EE_FORCE_INLINE ImU32 AdjustColorBrightness( ImU32 color, float multiplier )
    {
        return AdjustColorBrightness( ImColor( color ), multiplier );
    }

    //-------------------------------------------------------------------------
    // General helpers
    //-------------------------------------------------------------------------

    EE_SYSTEM_API void MakeTabVisible( char const* const pWindowName );

    EE_SYSTEM_API ImVec2 ClampToRect( ImRect const& rect, ImVec2 const& inPoint );

    // Returns the closest point on the rect border to the specified point
    EE_SYSTEM_API ImVec2 GetClosestPointOnRectBorder( ImRect const& rect, ImVec2 const& inPoint );

    // Is this a valid name ID character (i.e. A-Z, a-z, 0-9, _ )
    inline bool IsValidNameIDChar( ImWchar c )
    {
        return isalnum( c ) || c == '_';
    }

    inline int FilterNameIDChars( ImGuiInputTextCallbackData* data )
    {
        if ( IsValidNameIDChar( data->EventChar ) )
        {
            return 0;
        }
        return 1;
    }

    //-------------------------------------------------------------------------
    // Separators
    //-------------------------------------------------------------------------

    // Create a centered separator which can be immediately followed by a item
    EE_SYSTEM_API void PreSeparator( float width = 0 );

    // Create a centered separator which can be immediately followed by a item
    EE_SYSTEM_API void PostSeparator( float width = 0 );

    // Create a labeled separator: --- TEXT ---------------
    EE_SYSTEM_API void TextSeparator( char const* text, float preWidth = 10.0f, float totalWidth = 0 );

    // Draws a vertical separator on the current line and forces the next item to be on the same line. The size is the offset between the previous item and the next
    EE_SYSTEM_API void VerticalSeparator( ImVec2 const& size = ImVec2( -1, -1 ), ImColor const& color = 0 );

    //-------------------------------------------------------------------------
    // Basic Widgets
    //-------------------------------------------------------------------------

    // Draw a tooltip for the immediately preceding item
    EE_SYSTEM_API void ItemTooltip( const char* fmt, ... );

    // Draw a tooltip with a custom hover delay for the immediately preceding item
    EE_SYSTEM_API void ItemTooltipDelayed( float tooltipDelay, const char* fmt, ... );

    // For use with text widget
    EE_SYSTEM_API void TextTooltip( const char* fmt, ... );

    // Draw a button with an explicit icon
    EE_SYSTEM_API bool IconButton( char const* pIcon, char const* pLabel, ImVec4 const& iconColor = ImGui::GetStyle().Colors[ImGuiCol_Text], ImVec2 const& size = ImVec2( 0, 0 ) );

    // Draw a button with an explicit icon
    EE_FORCE_INLINE bool IconButton( char const* pIcon, char const* pLabel, Color const& iconColor = Color( ImGui::GetStyle().Colors[ImGuiCol_Text] ), ImVec2 const& size = ImVec2( 0, 0 ) )
    {
        return IconButton( pIcon, pLabel, ConvertColor( iconColor ).Value, size );
    }

    // Draw a colored button
    EE_SYSTEM_API bool ColoredButton( ImColor const& backgroundColor, ImColor const& foregroundColor, char const* label, ImVec2 const& size = ImVec2( 0, 0 ) );

    // Draw a colored button
    EE_FORCE_INLINE bool ColoredButton( Color backgroundColor, Color foregroundColor, char const* label, ImVec2 const& size = ImVec2( 0, 0 ) )
    {
        return ColoredButton( ConvertColor( backgroundColor ), ConvertColor( foregroundColor ), label, size );
    }

    // Draw a colored icon button
    EE_SYSTEM_API bool ColoredIconButton( ImColor const& backgroundColor, ImColor const& foregroundColor, ImVec4 const& iconColor, char const* pIcon, char const* label, ImVec2 const& size = ImVec2( 0, 0 ) );

    // Draw a colored icon button
    EE_FORCE_INLINE bool ColoredIconButton( Color backgroundColor, Color foregroundColor, Color iconColor, char const* pIcon, char const* label, ImVec2 const& size = ImVec2( 0, 0 ) )
    {
        return ColoredIconButton( ConvertColor( backgroundColor ), ConvertColor( foregroundColor ), ConvertColor( iconColor ), pIcon, label, size );
    }

    // Draws a flat button - a button with no background
    EE_SYSTEM_API bool FlatButton( char const* label, ImVec2 const& size = ImVec2( 0, 0 ) );

    // Draws a flat button - with a custom text color
    EE_FORCE_INLINE bool FlatButtonColored( ImVec4 const& foregroundColor, char const* label, ImVec2 const& size = ImVec2( 0, 0 ) )
    {
        ImGui::PushStyleColor( ImGuiCol_Button, 0 );
        ImGui::PushStyleColor( ImGuiCol_Text, foregroundColor );
        bool const result = ImGui::Button( label, size );
        ImGui::PopStyleColor( 2 );

        return result;
    }

    // Draw a colored icon button
    EE_SYSTEM_API bool FlatIconButton( char const* pIcon, char const* pLabel, ImVec4 const& iconColor = ImGui::GetStyle().Colors[ImGuiCol_Text], ImVec2 const& size = ImVec2( 0, 0 ) );

    // Draw a colored icon button
    EE_FORCE_INLINE bool FlatIconButton( char const* pIcon, char const* pLabel, Color iconColor = Color( ImGui::GetStyle().Colors[ImGuiCol_Text] ), ImVec2 const& size = ImVec2( 0, 0 ) )
    {
        return FlatIconButton( pIcon, pLabel, ConvertColor( iconColor ).Value, size );
    }

    // Draw an arrow between two points
    EE_SYSTEM_API void DrawArrow( ImDrawList* pDrawList, ImVec2 const& arrowStart, ImVec2 const& arrowEnd, ImU32 col, float arrowWidth, float arrowHeadWidth = 5.0f );

    // Draw an overlaid icon in a window, returns true if clicked
    EE_SYSTEM_API bool DrawOverlayIcon( ImVec2 const& iconPos, char icon[4], void* iconID, bool isSelected = false, ImColor const& selectedColor = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] );

    // Draw a basic spinner
    EE_SYSTEM_API bool DrawSpinner( char const* pLabel, ImColor const& color = ImGui::GetStyle().Colors[ImGuiCol_Text], float radius = 6.0f, float thickness = 3.0f );

    //-------------------------------------------------------------------------

    EE_SYSTEM_API bool InputFloat2( char const* pID, Float2& value, float width = -1, bool readOnly = false );
    EE_SYSTEM_API bool InputFloat3( char const* pID, Float3& value, float width = -1, bool readOnly = false );
    EE_SYSTEM_API bool InputFloat4( char const* pID, Float4& value, float width = -1, bool readOnly = false );
    EE_SYSTEM_API bool InputFloat4( char const* pID, Vector& value, float width = -1, bool readOnly = false );

    EE_SYSTEM_API bool InputTransform( char const* pID, Transform& value, float width = -1, bool readOnly = false );

    //-------------------------------------------------------------------------
    // Advanced widgets
    //-------------------------------------------------------------------------

    // A simple filter entry widget that allows you to string match to some entered text
    class EE_SYSTEM_API FilterWidget
    {
        constexpr static uint32_t const s_bufferSize = 255;

    public:

        enum Options : uint8_t
        {
            TakeInitialFocus = 0
        };

    public:

        // Draws the filter. Returns true if the filter has been updated
        bool DrawAndUpdate( TBitFlags<Options> options = TBitFlags<Options>() );

        inline void Clear();

        inline bool HasFilterSet() const { return !m_tokens.empty(); }

        inline TVector<String> const& GetFilterTokens() const { return m_tokens; }

        // Does a provided string match the current filter - the string copy is intentional!
        bool MatchesFilter( String string ); 

    private:

        char                m_buffer[s_bufferSize] = { 0 };
        TVector<String>     m_tokens;
    };

}
#endif