// A Wayland client whose buffers differ from their surfaces, for checking wp_viewport compositing.
//
// The toplevel is 800x600 in surface coordinates. Its buffer is 1920x1440 at twice that scale with
// a 160x120 magenta margin, and the viewport crops the margin away: source (160, 120, 1600, 1200),
// destination 800x600. Inside the crop are four quadrants, red, green, blue and white from the top
// left, each with a one buffer pixel black border. A subsurface at (600, 400) has a 100x100 buffer,
// cyan inside a 25 pixel magenta margin, cropped to (25, 25, 50, 50) and stretched to 150x75.
//
// Composited at its viewports on an 800x600 output, a frame shows the quadrants edge to edge, a
// cyan 150x75 rectangle at (600, 400) and no magenta anywhere.
//
// It redraws every frame, alternating two identical buffers, as a game would.
//
// Run it on Gamescope's socket: WAYLAND_DISPLAY=gamescope-0 viewport_client

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace
{
	constexpr int32_t k_nSurfaceWidth = 800;
	constexpr int32_t k_nSurfaceHeight = 600;
	constexpr int32_t k_nBufferScale = 2;
	constexpr int32_t k_nMarginX = 160;
	constexpr int32_t k_nMarginY = 120;

	constexpr int32_t k_nSubX = 600;
	constexpr int32_t k_nSubY = 400;
	constexpr int32_t k_nSubBuffer = 100;
	constexpr int32_t k_nSubMargin = 25;
	constexpr int32_t k_nSubWidth = 150;
	constexpr int32_t k_nSubHeight = 75;

	constexpr uint32_t k_uMagenta = 0xffff00ff;
	constexpr uint32_t k_uCyan = 0xff00ffff;
	constexpr uint32_t k_uBlack = 0xff000000;

	struct Globals
	{
		wl_compositor *pCompositor = nullptr;
		wl_subcompositor *pSubcompositor = nullptr;
		wl_shm *pShm = nullptr;
		xdg_wm_base *pWmBase = nullptr;
		wp_viewporter *pViewporter = nullptr;
		bool bConfigured = false;
	};

	void OnGlobal( void *pData, wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
	{
		Globals *pGlobals = static_cast<Globals *>( pData );
		if ( !strcmp( pInterface, wl_compositor_interface.name ) )
			pGlobals->pCompositor = static_cast<wl_compositor *>( wl_registry_bind( pRegistry, uName, &wl_compositor_interface, 4 ) );
		else if ( !strcmp( pInterface, wl_subcompositor_interface.name ) )
			pGlobals->pSubcompositor = static_cast<wl_subcompositor *>( wl_registry_bind( pRegistry, uName, &wl_subcompositor_interface, 1 ) );
		else if ( !strcmp( pInterface, wl_shm_interface.name ) )
			pGlobals->pShm = static_cast<wl_shm *>( wl_registry_bind( pRegistry, uName, &wl_shm_interface, 1 ) );
		else if ( !strcmp( pInterface, xdg_wm_base_interface.name ) )
			pGlobals->pWmBase = static_cast<xdg_wm_base *>( wl_registry_bind( pRegistry, uName, &xdg_wm_base_interface, 1 ) );
		else if ( !strcmp( pInterface, wp_viewporter_interface.name ) )
			pGlobals->pViewporter = static_cast<wp_viewporter *>( wl_registry_bind( pRegistry, uName, &wp_viewporter_interface, 1 ) );
	}

	void OnGlobalRemove( void *, wl_registry *, uint32_t )
	{
	}

	const wl_registry_listener s_RegistryListener = { OnGlobal, OnGlobalRemove };

	void OnPing( void *, xdg_wm_base *pWmBase, uint32_t uSerial )
	{
		xdg_wm_base_pong( pWmBase, uSerial );
	}

	const xdg_wm_base_listener s_WmBaseListener = { OnPing };

	void OnSurfaceConfigure( void *pData, xdg_surface *pXdgSurface, uint32_t uSerial )
	{
		xdg_surface_ack_configure( pXdgSurface, uSerial );
		static_cast<Globals *>( pData )->bConfigured = true;
	}

	const xdg_surface_listener s_XdgSurfaceListener = { OnSurfaceConfigure };

	void OnToplevelConfigure( void *, xdg_toplevel *, int32_t, int32_t, wl_array * )
	{
	}

	void OnToplevelClose( void *, xdg_toplevel * )
	{
	}

	const xdg_toplevel_listener s_ToplevelListener = { OnToplevelConfigure, OnToplevelClose };

	struct Redraw
	{
		wl_surface *pSurface = nullptr;
		wl_buffer *pBuffers[ 2 ] = {};
		uint32_t uFrame = 0;
	};

	void Draw( Redraw *pRedraw );

	void OnFrameDone( void *pData, wl_callback *pCallback, uint32_t )
	{
		wl_callback_destroy( pCallback );
		Draw( static_cast<Redraw *>( pData ) );
	}

	const wl_callback_listener s_FrameListener = { OnFrameDone };

	void Draw( Redraw *pRedraw )
	{
		wl_callback_add_listener( wl_surface_frame( pRedraw->pSurface ), &s_FrameListener, pRedraw );
		wl_surface_attach( pRedraw->pSurface, pRedraw->pBuffers[ pRedraw->uFrame++ % 2 ], 0, 0 );
		wl_surface_damage_buffer( pRedraw->pSurface, 0, 0, INT32_MAX, INT32_MAX );
		wl_surface_commit( pRedraw->pSurface );
	}

	// An ARGB8888 shm buffer, filled by pfnPixel( x, y ).
	template <typename Pixel>
	wl_buffer *CreateBuffer( wl_shm *pShm, int32_t nWidth, int32_t nHeight, Pixel pfnPixel )
	{
		const int32_t nStride = nWidth * 4;
		const size_t uSize = size_t( nStride ) * nHeight;
		int nFd = memfd_create( "viewport_client", MFD_CLOEXEC );
		if ( nFd < 0 || ftruncate( nFd, off_t( uSize ) ) < 0 )
			return nullptr;

		void *pMapping = mmap( nullptr, uSize, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, 0 );
		if ( pMapping == MAP_FAILED )
		{
			close( nFd );
			return nullptr;
		}
		uint32_t *pPixels = static_cast<uint32_t *>( pMapping );
		for ( int32_t y = 0; y < nHeight; y++ )
		{
			for ( int32_t x = 0; x < nWidth; x++ )
				pPixels[ y * nWidth + x ] = pfnPixel( x, y );
		}
		munmap( pMapping, uSize );

		wl_shm_pool *pPool = wl_shm_create_pool( pShm, nFd, int32_t( uSize ) );
		wl_buffer *pBuffer = wl_shm_pool_create_buffer( pPool, 0, nWidth, nHeight, nStride, WL_SHM_FORMAT_ARGB8888 );
		wl_shm_pool_destroy( pPool );
		close( nFd );
		return pBuffer;
	}

	uint32_t ToplevelPixel( int32_t x, int32_t y )
	{
		const int32_t nCropWidth = k_nSurfaceWidth * k_nBufferScale;
		const int32_t nCropHeight = k_nSurfaceHeight * k_nBufferScale;
		const int32_t nX = x - k_nMarginX;
		const int32_t nY = y - k_nMarginY;
		if ( nX < 0 || nY < 0 || nX >= nCropWidth || nY >= nCropHeight )
			return k_uMagenta;

		const int32_t nQuadX = nX % ( nCropWidth / 2 );
		const int32_t nQuadY = nY % ( nCropHeight / 2 );
		if ( nQuadX == 0 || nQuadY == 0 || nQuadX == nCropWidth / 2 - 1 || nQuadY == nCropHeight / 2 - 1 )
			return k_uBlack;

		const bool bRight = nX >= nCropWidth / 2;
		const bool bBottom = nY >= nCropHeight / 2;
		if ( !bBottom )
			return bRight ? 0xff00ff00 : 0xffff0000;
		return bRight ? 0xffffffff : 0xff0000ff;
	}

	uint32_t SubsurfacePixel( int32_t x, int32_t y )
	{
		const bool bInside = x >= k_nSubMargin && y >= k_nSubMargin &&
			x < k_nSubBuffer - k_nSubMargin && y < k_nSubBuffer - k_nSubMargin;
		return bInside ? k_uCyan : k_uMagenta;
	}
}

int main()
{
	wl_display *pDisplay = wl_display_connect( nullptr );
	if ( !pDisplay )
	{
		fprintf( stderr, "viewport_client: no Wayland display\n" );
		return 1;
	}

	Globals globals;
	wl_registry *pRegistry = wl_display_get_registry( pDisplay );
	wl_registry_add_listener( pRegistry, &s_RegistryListener, &globals );
	wl_display_roundtrip( pDisplay );
	if ( !globals.pCompositor || !globals.pSubcompositor || !globals.pShm || !globals.pWmBase || !globals.pViewporter )
	{
		fprintf( stderr, "viewport_client: the compositor lacks wl_compositor, wl_subcompositor, wl_shm, xdg_wm_base or wp_viewporter\n" );
		return 1;
	}
	xdg_wm_base_add_listener( globals.pWmBase, &s_WmBaseListener, nullptr );

	wl_surface *pSurface = wl_compositor_create_surface( globals.pCompositor );
	xdg_surface *pXdgSurface = xdg_wm_base_get_xdg_surface( globals.pWmBase, pSurface );
	xdg_surface_add_listener( pXdgSurface, &s_XdgSurfaceListener, &globals );
	xdg_toplevel *pToplevel = xdg_surface_get_toplevel( pXdgSurface );
	xdg_toplevel_add_listener( pToplevel, &s_ToplevelListener, nullptr );
	xdg_toplevel_set_title( pToplevel, "viewport_client" );
	xdg_toplevel_set_app_id( pToplevel, "viewport_client" );
	wl_surface_commit( pSurface );
	while ( !globals.bConfigured )
	{
		if ( wl_display_dispatch( pDisplay ) < 0 )
			return 1;
	}

	wl_surface *pSubSurface = wl_compositor_create_surface( globals.pCompositor );
	wl_subsurface *pSubsurface = wl_subcompositor_get_subsurface( globals.pSubcompositor, pSubSurface, pSurface );
	wl_subsurface_set_position( pSubsurface, k_nSubX, k_nSubY );
	wl_subsurface_set_desync( pSubsurface );

	wp_viewport *pSubViewport = wp_viewporter_get_viewport( globals.pViewporter, pSubSurface );
	wp_viewport_set_source( pSubViewport, wl_fixed_from_int( k_nSubMargin ), wl_fixed_from_int( k_nSubMargin ),
		wl_fixed_from_int( k_nSubBuffer - 2 * k_nSubMargin ), wl_fixed_from_int( k_nSubBuffer - 2 * k_nSubMargin ) );
	wp_viewport_set_destination( pSubViewport, k_nSubWidth, k_nSubHeight );
	wl_buffer *pSubBuffer = CreateBuffer( globals.pShm, k_nSubBuffer, k_nSubBuffer, SubsurfacePixel );
	wl_surface_attach( pSubSurface, pSubBuffer, 0, 0 );
	wl_surface_damage_buffer( pSubSurface, 0, 0, INT32_MAX, INT32_MAX );
	wl_surface_commit( pSubSurface );

	wp_viewport *pViewport = wp_viewporter_get_viewport( globals.pViewporter, pSurface );
	wp_viewport_set_source( pViewport, wl_fixed_from_int( k_nMarginX ), wl_fixed_from_int( k_nMarginY ),
		wl_fixed_from_int( k_nSurfaceWidth * k_nBufferScale ), wl_fixed_from_int( k_nSurfaceHeight * k_nBufferScale ) );
	wp_viewport_set_destination( pViewport, k_nSurfaceWidth, k_nSurfaceHeight );
	Redraw redraw;
	redraw.pSurface = pSurface;
	for ( wl_buffer *&pBuffer : redraw.pBuffers )
	{
		pBuffer = CreateBuffer( globals.pShm,
			k_nSurfaceWidth * k_nBufferScale + 2 * k_nMarginX, k_nSurfaceHeight * k_nBufferScale + 2 * k_nMarginY, ToplevelPixel );
		if ( !pBuffer || !pSubBuffer )
		{
			fprintf( stderr, "viewport_client: cannot create the shm buffers\n" );
			return 1;
		}
	}
	Draw( &redraw );

	fprintf( stderr, "viewport_client: mapped\n" );
	while ( wl_display_dispatch( pDisplay ) >= 0 )
	{
	}
	return 0;
}
