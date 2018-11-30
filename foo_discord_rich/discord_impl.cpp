#include <stdafx.h>
#include "discord_impl.h"

#include <config.h>

#include <ctime>

namespace drp::internal
{

PresenceData::PresenceData()
{
    memset( &presence, 0, sizeof( presence ) );
    presence.state = state.c_str();
    presence.details = details.c_str();
}

PresenceData::PresenceData( const PresenceData& other )
{
    metadb = other.metadb;
    state = other.state;
    details = other.details;
    trackLength = other.trackLength;

    memcpy( &presence, &other.presence, sizeof( presence ) );
    presence.state = state.c_str();
    presence.details = details.c_str();
}

PresenceData& PresenceData::operator=( const PresenceData& other )
{
    if ( this != &other )
    {
        metadb = other.metadb;
        state = other.state;
        details = other.details;
        trackLength = other.trackLength;

        memcpy( &presence, &other.presence, sizeof( presence ) );
        presence.state = state.c_str();
        presence.details = details.c_str();
    }

    return *this;
}

bool PresenceData::operator==( const PresenceData& other )
{
    auto areStringsSame = []( const char* a, const char* b ) {
        return ( ( a == b ) || ( a && b && !strcmp( a, b ) ) );
    };

    return ( areStringsSame( presence.state, other.presence.state )
             && areStringsSame( presence.details, other.presence.details )
             && areStringsSame( presence.largeImageKey, other.presence.largeImageKey )
             && areStringsSame( presence.largeImageText, other.presence.largeImageText )
             && areStringsSame( presence.smallImageKey, other.presence.smallImageKey )
             && areStringsSame( presence.smallImageText, other.presence.smallImageText )
             && trackLength == other.trackLength );
}

bool PresenceData::operator!=( const PresenceData& other )
{
    return operator==( other );
}

} // namespace drp::internal

namespace drp
{

PresenceModifier::PresenceModifier( DiscordHandler& parent,
                                    const drp::internal::PresenceData& presenceData )
    : parent_( parent )
    , presenceData_( presenceData )
{
}

PresenceModifier::~PresenceModifier()
{
    if ( parent_.presenceData_ != presenceData_ )
    {
        parent_.presenceData_ = presenceData_;
    }

    if ( isCleared_ )
    {
        parent_.ClearPresence();
    }
    else
    {
        parent_.SendPresense();
    }
}

void PresenceModifier::UpdateImage()
{
    auto& pd = presenceData_;

    switch ( static_cast<config::ImageSetting>( config::g_imageSettings.GetSavedValue() ) )
    {
    case config::ImageSetting::Light:
    {
        pd.presence.largeImageKey = "foobar2000";
        break;
    }
    case config::ImageSetting::Dark:
    {
        pd.presence.largeImageKey = "foobar2000-dark";
        break;
    }
    case config::ImageSetting::Disabled:
    {
        pd.presence.largeImageKey = nullptr;
        break;
    }
    }
}

void PresenceModifier::UpdateTrack( metadb_handle_ptr metadb )
{
    auto& pd = presenceData_;

    pd.state.reset();
    pd.details.reset();
    pd.trackLength = 0;

    if ( metadb.is_valid() )
    { // Need to save in case refresh requested on settings update
        pd.metadb = metadb;
    }

    auto pc = playback_control::get();
    auto queryData = [&pc, metadb = pd.metadb]( const pfc::string8_fast& query ) {
        titleformat_object::ptr tf;
        titleformat_compiler::get()->compile_safe( tf, query );
        pfc::string8_fast result;

        if ( pc->is_playing() )
        {
            metadb_handle_ptr dummyHandle;
            pc->playback_format_title_ex( dummyHandle, nullptr, result, tf, nullptr, playback_control::display_level_all );
        }
        else if ( metadb.is_valid() )
        {
            metadb->format_title( nullptr, result, tf, nullptr );
        }

        return result;
    };

    pd.state = queryData( config::g_stateQuery );
    pd.state.truncate( 127 );
    pd.details = queryData( config::g_detailsQuery );
    pd.details.truncate( 127 );

    pfc::string8_fast lengthStr = queryData( "[%length_seconds_fp%]" );
    pd.trackLength = ( lengthStr.is_empty() ? 0 : stold( std::string( lengthStr ) ) );

    pfc::string8_fast durationStr = queryData( "[%playback_time_seconds%]" );

    pd.presence.state = pd.state.c_str();
    pd.presence.details = pd.details.c_str();
    UpdateDuration( durationStr.is_empty() ? 0 : stold( std::string( durationStr ) ) );
}

void PresenceModifier::UpdateDuration( double time )
{
    auto& pd = presenceData_;
    auto pc = playback_control::get();
    const config::TimeSetting timeSetting = ( ( pd.trackLength && pc->is_playing() && !pc->is_paused() )
                                                  ? static_cast<config::TimeSetting>( config::g_timeSettings.GetSavedValue() )
                                                  : config::TimeSetting::Disabled );
    switch ( timeSetting )
    {
    case config::TimeSetting::Elapsed:
    {
        pd.presence.startTimestamp = std::time( nullptr ) - std::llround( time );
        pd.presence.endTimestamp = 0;

        break;
    }
    case config::TimeSetting::Remaining:
    {
        pd.presence.startTimestamp = 0;
        pd.presence.endTimestamp = std::time( nullptr ) + std::max<uint64_t>( 0, std::llround( pd.trackLength - time ) );

        break;
    }
    case config::TimeSetting::Disabled:
    {
        pd.presence.startTimestamp = 0;
        pd.presence.endTimestamp = 0;

        break;
    }
    }
}

void PresenceModifier::DisableDuration()
{
    auto& pd = presenceData_;
    pd.presence.startTimestamp = 0;
    pd.presence.endTimestamp = 0;
}

void PresenceModifier::Clear()
{
    isCleared_ = true;
}

DiscordHandler& DiscordHandler::GetInstance()
{
    static DiscordHandler discordHandler;
    return discordHandler;
}

void DiscordHandler::Initialize()
{
    DiscordEventHandlers handlers;
    memset( &handlers, 0, sizeof( handlers ) );

    handlers.ready = OnReady;
    handlers.disconnected = OnDisconnected;
    handlers.errored = OnErrored;

    Discord_Initialize( "507982587416018945", &handlers, 1, nullptr );
    Discord_RunCallbacks();

    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.Clear(); ///< we don't want to activate presence yet
}

void DiscordHandler::Finalize()
{
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordHandler::OnSettingsChanged()
{
    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.UpdateTrack();
}

void DiscordHandler::SendPresense()
{
    if ( config::g_isEnabled )
    {
        Discord_UpdatePresence( &presenceData_.presence );
    }
    else
    {
        Discord_ClearPresence();
    }
    Discord_RunCallbacks();
}

void DiscordHandler::ClearPresence()
{
    Discord_ClearPresence();
    Discord_RunCallbacks();
}

PresenceModifier DiscordHandler::GetPresenceModifier()
{
    return PresenceModifier( *this, presenceData_ );
}

void DiscordHandler::OnReady( const DiscordUser* request )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": connected to " << ( request->username ? request->username : "<null>" );
}

void DiscordHandler::OnDisconnected( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": disconnected with code " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

void DiscordHandler::OnErrored( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": error " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

} // namespace drp
