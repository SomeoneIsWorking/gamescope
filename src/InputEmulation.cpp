#if HAVE_LIBEIS

#include <libeis.h>

#include <cstdio>
#include <cstring>

#include "backend.h"
#include "InputEmulation.h"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"

static LogScope gamescope_ei("gamescope_ei");

// A client of this name points over the PipeWire stream's frame, not the focused window: its
// absolute coordinates are 0 to k_nPictureSpan across each side of the frame.
static constexpr const char *k_szPictureClient = "gamescope-picture-pointer";
static constexpr int32_t k_nPictureSpan = 65536;
static int s_nPictureDeviceTag;

namespace gamescope
{
    GamescopeInputServer::GamescopeInputServer()
    {
    }

    GamescopeInputServer::~GamescopeInputServer()
    {
        eis_unref( m_pEis );
        m_pEis = nullptr;
        // We don't own the fd.
        m_nFd = -1;
    }

    bool GamescopeInputServer::Init( const char *pszSocketPath )
    {
        if ( !pszSocketPath || !*pszSocketPath )
        {
            gamescope_ei.errorf( "No Gamescope EIS socket path!" );
            return false;
        }

        m_pEis = eis_new( nullptr );
        if ( !m_pEis )
        {
            gamescope_ei.errorf( "Failed to create eis." );
            return false;
        }

        if ( eis_setup_backend_socket( m_pEis, pszSocketPath ) != 0 )
        {
            gamescope_ei.errorf( "Failed to init eis: %s", strerror( errno ) );
            return false;
        }

        m_nFd = eis_get_fd( m_pEis );
        if ( m_nFd < 0 )
        {
            gamescope_ei.errorf( "Failed to get eis fd" );
            return false;
        }

        return true;
    }

    int GamescopeInputServer::GetFD()
    {
        return m_nFd;
    }

    void GamescopeInputServer::OnPollIn()
    {
        static uint32_t s_uSequence = 0;

        eis_dispatch( m_pEis );

        while ( eis_event *pEisEvent = eis_get_event( m_pEis ) )
        {
            switch ( eis_event_get_type( pEisEvent ) )
            {
                case EIS_EVENT_CLIENT_CONNECT:
                {
                    eis_client *pClient = eis_event_get_client( pEisEvent );
                    eis_client_connect( pClient );

                    eis_seat *pSeat = eis_client_new_seat( pClient, "chair" );
                    eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_POINTER );
                    eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_POINTER_ABSOLUTE );
                    eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_KEYBOARD );
                    //eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_TOUCH );
                    eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_BUTTON );
                    eis_seat_configure_capability( pSeat, EIS_DEVICE_CAP_SCROLL );
                    eis_seat_add( pSeat );
                    // Unref it now that we no longer need it, and gave it over to eis.
                    eis_seat_unref( pSeat );
                }
                break;

                case EIS_EVENT_CLIENT_DISCONNECT:
                {
                    eis_client *pClient = eis_event_get_client( pEisEvent );
                    eis_client_disconnect( pClient );
                }
                break;

                case EIS_EVENT_SEAT_BIND:
                {
                    eis_client *pClient = eis_event_get_client( pEisEvent );
                    eis_seat *pSeat = eis_event_get_seat( pEisEvent );

                    bool bWantsDevice = eis_event_seat_has_capability( pEisEvent, EIS_DEVICE_CAP_POINTER ) || 
                                        eis_event_seat_has_capability( pEisEvent, EIS_DEVICE_CAP_POINTER_ABSOLUTE ) ||
                                        eis_event_seat_has_capability( pEisEvent, EIS_DEVICE_CAP_BUTTON ) ||
                                        eis_event_seat_has_capability( pEisEvent, EIS_DEVICE_CAP_SCROLL ) ||
                                        eis_event_seat_has_capability( pEisEvent, EIS_DEVICE_CAP_KEYBOARD );

                    bool bHasDevice = eis_client_get_user_data( pClient ) != nullptr;

                    const char *pszClientName = eis_client_get_name( pClient );
                    const bool bPicture = pszClientName && strcmp( pszClientName, k_szPictureClient ) == 0;

                    if ( bWantsDevice && !bHasDevice )
                    {
                        eis_device *pVirtualInput = eis_seat_new_device( pSeat );
                        eis_device_configure_name( pVirtualInput, bPicture ? "Gamescope Picture Pointer" : "Gamescope Virtual Input" );
                        eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_POINTER );
                        eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_POINTER_ABSOLUTE );
                        eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_BUTTON );
                        eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_SCROLL );
                        eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_KEYBOARD );
                        // Can add this someday if we want it.
                        //eis_device_configure_capability( pVirtualInput, EIS_DEVICE_CAP_TOUCH );

                        eis_region *pVirtualInputRegion = eis_device_new_region( pVirtualInput );
                        eis_region_set_mapping_id( pVirtualInputRegion, bPicture ? "gamescope-picture" : "Mr. Worldwide" );
                        if ( bPicture )
                        {
                            eis_region_set_size( pVirtualInputRegion, k_nPictureSpan, k_nPictureSpan );
                            eis_device_set_user_data( pVirtualInput, &s_nPictureDeviceTag );
                        }
                        else
                        {
                            eis_region_set_size( pVirtualInputRegion, INT32_MAX, INT32_MAX );
                        }
                        eis_region_set_offset( pVirtualInputRegion, 0, 0 );
                        eis_region_add( pVirtualInputRegion );
                        // We don't want this anymore, but pVirtualInput can own it
                        eis_region_unref( pVirtualInputRegion );

                        eis_device_add( pVirtualInput );
                        eis_device_resume( pVirtualInput );
                        if ( !eis_client_is_sender( pClient ) )
                            eis_device_start_emulating( pVirtualInput, ++s_uSequence );

                        // We have a ref on pVirtualInput, store that in pClient's userdata so we can remove device later.
                        eis_client_set_user_data( pClient, (void *) pVirtualInput );
                    }
                    else if ( !bWantsDevice && bHasDevice )
                    {
                        eis_device *pDevice = (eis_device *) eis_client_get_user_data( pClient );
                        eis_device_remove( pDevice );
                        eis_device_unref( pDevice );
                        eis_client_set_user_data( pClient, nullptr );
                    }
                }
                break;

                case EIS_EVENT_DEVICE_CLOSED:
                {
                    eis_client *pClient = eis_event_get_client( pEisEvent );
                    eis_device *pDevice = eis_event_get_device( pEisEvent );

                    // Remove the device from our tracking on the client.
                    eis_device_remove( pDevice );
                    eis_device_unref( pDevice );
                    eis_client_set_user_data( pClient, nullptr );
                }
                break;

                case EIS_EVENT_DEVICE_START_EMULATING:
                case EIS_EVENT_DEVICE_STOP_EMULATING:
                {
                    // Don't care.
                }
                break;

                case EIS_EVENT_POINTER_MOTION:
                {
                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    wlserver_lock();
                    wlserver_mousemotion( eis_event_pointer_get_dx( pEisEvent ), eis_event_pointer_get_dy( pEisEvent ), ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
                {
                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    double flX = eis_event_pointer_get_absolute_x( pEisEvent );
                    double flY = eis_event_pointer_get_absolute_y( pEisEvent );
                    if ( eis_device_get_user_data( eis_event_get_device( pEisEvent ) ) == &s_nPictureDeviceTag )
                    {
                        // Through the stream frame's own fit of the window, as touch goes through the output's.
                        std::optional<PipewirePointerTransform> oTransform = pipewire_pointer_transform();
                        if ( !oTransform )
                            break;
                        flX = ( flX / k_nPictureSpan * oTransform->uWidth + oTransform->flOffsetX ) * oTransform->flScaleX;
                        flY = ( flY / k_nPictureSpan * oTransform->uHeight + oTransform->flOffsetY ) * oTransform->flScaleY;
                    }

                    wlserver_lock();
                    wlserver_mousewarp( flX, flY, ++s_uSequence, true );
                    wlserver_unlock();
                }
                break;

                case EIS_EVENT_BUTTON_BUTTON:
                {
                    wlserver_lock();
                    wlserver_mousebutton( eis_event_button_get_button( pEisEvent ), eis_event_button_get_is_press( pEisEvent ), ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case EIS_EVENT_SCROLL_DELTA:
                {
                    wlserver_lock();
                    wlserver_mousewheel( eis_event_scroll_get_dx( pEisEvent ), eis_event_scroll_get_dy( pEisEvent ), ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case EIS_EVENT_SCROLL_DISCRETE:
                {
                    m_flScrollAccum[0] += eis_event_scroll_get_discrete_dx( pEisEvent ) / 120.0;
                    m_flScrollAccum[1] += eis_event_scroll_get_discrete_dy( pEisEvent ) / 120.0;
                }
                break;

                case EIS_EVENT_KEYBOARD_KEY:
                {
                    wlserver_lock();
                    wlserver_key( eis_event_keyboard_get_key( pEisEvent ), eis_event_keyboard_get_key_is_press( pEisEvent ), ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case EIS_EVENT_TOUCH_DOWN:
                case EIS_EVENT_TOUCH_MOTION:
                case EIS_EVENT_TOUCH_UP:
                {
                    // Touch not implemented yet.
                    gamescope_ei.errorf( "No touch support yet! How did you get here?" );
                }
                break;

                case EIS_EVENT_FRAME:
                {
                    double flScrollX = m_flScrollAccum[0];
                    double flScrollY = m_flScrollAccum[1];
                    m_flScrollAccum[0] = 0.0;
                    m_flScrollAccum[1] = 0.0;

                    if ( flScrollX == 0.0 && flScrollY == 0.0 )
                        break;

                    wlserver_lock();
                    wlserver_mousewheel( flScrollX, flScrollY, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                default:
                {
                    gamescope_ei.errorf( "Unhandled libei event!" );
                }
                break;
            }

            eis_event_unref( pEisEvent );
        }
    }
}
#endif
