#pragma comment(linker, "/dll")
#pragma comment(linker, "/ENTRY:\"DllEntryPoint\"") // define entry point cause no C Lib
#pragma comment(linker, "/export:DirectDrawCreate=_DirectDrawCreate@12")
#pragma comment(linker, "/NODEFAULTLIB") // specifically no C runtime lib (for no real reason)
#pragma comment( lib, "kernel32" )
#pragma comment( lib, "user32" )
#pragma comment( lib, "gdi32" )

#include <windows.h>
#include <ddraw.h>

struct {
	BITMAPINFOHEADER bmiHeader;
	RGBQUAD bmiColors[256]; 
} bmi;

HWND hwnd_main;
void* pvBmpBits;
HDC hdc_offscreen;
const DWORD width = 640;
const DWORD height = 480;
HBITMAP hOldBitmap; // for cleanup

WNDPROC ButtonWndProc_original;

extern const DWORD dd_vtbl[];
extern const DWORD dds_vtbl[];
extern const DWORD ddp_vtbl[];
const DWORD* const IDDraw = dd_vtbl;
const DWORD* const IDDSurf = dds_vtbl;
const DWORD* const IDDPal = ddp_vtbl;

// real ddraw for fullscreen
IDirectDraw* ddraw;
IDirectDrawSurface* dds_primary = NULL;

#pragma intrinsic( strcat )
HRESULT GoFullscreen( void )
{
	HMODULE ddraw_dll; 
	DDSURFACEDESC ddsd;
	typedef HRESULT (__stdcall* DIRECTDRAWCREATE)( GUID*, IDirectDraw**, IUnknown* ); 

	// load ddraw.dll from system32 dir
	char szPath[ MAX_PATH ];
	if( GetSystemDirectory( szPath, MAX_PATH - 10 ))
	{
		strcat( szPath, "\\ddraw.dll" );
		ddraw_dll = LoadLibrary( szPath );

		if( ddraw_dll != NULL)
		{
			DIRECTDRAWCREATE pfnDirectDrawCreate = (DIRECTDRAWCREATE) GetProcAddress( ddraw_dll, "DirectDrawCreate" );
			if( pfnDirectDrawCreate != NULL )
			{
				if( SUCCEEDED( pfnDirectDrawCreate( (GUID*)0, &ddraw, NULL ) ) )
				{ 
					if( SUCCEEDED( ddraw->lpVtbl->SetCooperativeLevel( ddraw, hwnd_main, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE ) ) )
					{
						if( SUCCEEDED( ddraw->lpVtbl->SetDisplayMode( ddraw, width, height, 32 ) ) )
						{
							RtlSecureZeroMemory(&ddsd,sizeof(ddsd));
							ddsd.dwSize = sizeof(ddsd);
							ddsd.dwFlags = DDSD_CAPS;
							ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
							if( SUCCEEDED( ddraw->lpVtbl->CreateSurface( ddraw, &ddsd, &dds_primary, NULL)))
							{
								return DD_OK;
							}
							ddraw->lpVtbl->RestoreDisplayMode( ddraw );
						}
						ddraw->lpVtbl->SetCooperativeLevel( ddraw, hwnd_main, DDSCL_NORMAL );
					}
					ddraw->lpVtbl->Release( ddraw );
				}
				FreeLibrary( ddraw_dll );
			}
		}
	}
	return DDERR_GENERIC;
}

BOOL CheckFullscreen()
{
	if( dds_primary == NULL ) return FALSE;
	if( SUCCEEDED( dds_primary->lpVtbl->IsLost( dds_primary ) ) ) return TRUE;
	if( SUCCEEDED( dds_primary->lpVtbl->Restore( dds_primary ) ) ) return TRUE;
	return FALSE;
}


// HACK // 
// as a work-around for a unkown problem...
// ...I wish to force a redraw when the "Player Profile" screen exits
// however, it is easier to hook a system class ( button ) than a local class ( SDlgDialog )
// so instead we'll force a redraw anytime a button is destroyed.
LRESULT __stdcall ButtonWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	if( msg == WM_DESTROY ) RedrawWindow( NULL, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN );
	return ButtonWndProc_original( hwnd, msg, wParam, lParam );
}


void ToScreen( void )
{
	HDC hdc;
	HWND hwnd;
	RECT rc;
	DWORD* p;
	int i;
	RGBQUAD quad;
	COLORREF clear_color;

	CheckFullscreen();

	hwnd = FindWindowEx( HWND_DESKTOP, NULL, "SDlgDialog", NULL ); // detect mixed gdi/ddraw screen
	if( hwnd == NULL ) // in-game (ddraw only)
	{  
		// simpler/faster blit that also keeps screen shots (mostly) working...
		// no screen flash when ss is taken?
		hdc = GetDC( hwnd_main );
		BitBlt( hdc, 0, 0, 640, 480, hdc_offscreen, 0, 0, SRCCOPY );
		ReleaseDC( hwnd_main, hdc );
		return;
	}

	// hijack one of the palette entries to be a clear color :-(
	GetDIBColorTable( hdc_offscreen, 0xFE, 1, &quad );
	clear_color = RGB( quad.rgbRed, quad.rgbGreen, quad.rgbBlue );
	
	hdc = GetDC( hwnd_main );
	GdiTransparentBlt( hdc, 0, 0, 640, 480, hdc_offscreen, 0, 0, 640, 480, clear_color );
	ReleaseDC( hwnd_main, hdc );

	// blast it out to all top-level SDlgDialog windows... the realwtf
	do
	{ 	
		GetWindowRect( hwnd, &rc );
		hdc = GetDCEx( hwnd, NULL, DCX_PARENTCLIP | DCX_CACHE );
		GdiTransparentBlt( hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
				hdc_offscreen, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
				clear_color 
			);
		ReleaseDC( hwnd, hdc );
		hwnd = FindWindowEx( HWND_DESKTOP, hwnd, "SDlgDialog", NULL );
	} while( hwnd != NULL );

	// erase ( breaks screen shots, use alt+prtnscr instead )
	p = (DWORD*) pvBmpBits;
	for( i = 0; i < 640 * 480 / 4; i++ ) p[i] = 0xFEFEFEFE;

	return;
}


BOOL __stdcall DllEntryPoint( HINSTANCE hDll, DWORD dwReason, LPVOID lpvReserved )
{
	HBITMAP hBitmap;
	WNDCLASS wc;
	HINSTANCE hInst;

	if( dwReason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls( hDll );

		// create offscreen drawing surface
		bmi.bmiHeader.biSize = sizeof( BITMAPINFOHEADER ); 
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = 0 - height;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 8;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biSizeImage = 0;
		bmi.bmiHeader.biXPelsPerMeter = 0;
		bmi.bmiHeader.biYPelsPerMeter = 0;
		bmi.bmiHeader.biClrUsed = 0;
		bmi.bmiHeader.biClrImportant = 0; 
		hBitmap = CreateDIBSection( NULL, (BITMAPINFO*) &bmi, DIB_RGB_COLORS, &pvBmpBits, NULL, 0 );
		hdc_offscreen = CreateCompatibleDC( NULL );
		hOldBitmap = (HBITMAP) SelectObject( hdc_offscreen, hBitmap );

		// super class 
		hInst = GetModuleHandle( NULL );
		GetClassInfo( NULL, "Button", &wc );
		wc.hInstance = hInst;
		ButtonWndProc_original = wc.lpfnWndProc;
		wc.lpfnWndProc = ButtonWndProc;
		RegisterClass( &wc );
	}

	if( dwReason == DLL_PROCESS_DETACH )
	{
		// todo: delete dibsection...
		hOldBitmap = (HBITMAP) SelectObject( hdc_offscreen, hOldBitmap );
	}
	return TRUE;
}


#pragma intrinsic( _byteswap_ulong ) // i486+
HRESULT __stdcall ddp_SetEntries( void* This, DWORD dwFlags, DWORD dwStartingEntry, DWORD dwCount, LPPALETTEENTRY lpEntries )
{
	static RGBQUAD colors[256];
	int i;
	for( i = 0; i < 256; i++ )
	{ // convert 0xFFBBGGRR to 0x00RRGGBB
		*((DWORD*)&colors[i]) = _byteswap_ulong( *(DWORD*)&lpEntries[i] ) >> 8;
	}

	SetDIBColorTable( hdc_offscreen, 0, 256, colors );

	ToScreen(); // animate palette

	// HACK // 
	// not drawing main menu after movie? 
	// this still isn't fix 100% ...
	InvalidateRect( hwnd_main, NULL, TRUE ); 
	return 0;
}


HRESULT __stdcall dd_SetDisplayMode( void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP )
{ 
	return 0; 
}

HRESULT __stdcall dds_Lock( void* This, LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent )
{
	GdiFlush();
	lpDDSurfaceDesc->lPitch = 640;
	lpDDSurfaceDesc->lpSurface = pvBmpBits;

	return 0;
}

HRESULT __stdcall dds_Unlock( void* This, LPVOID lpSurfMemPtr )
{
	ToScreen();
	return 0;
}

HRESULT __stdcall dd_CreateSurface( void* This, LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter )
{	
	*lplpDDSurface = (LPDIRECTDRAWSURFACE) &IDDSurf;
	GoFullscreen();
	return 0;
}

HRESULT __stdcall dd_CreatePalette( void* This, DWORD dwFlags, LPPALETTEENTRY lpColorTable, LPDIRECTDRAWPALETTE* lplpDDPalette, IUnknown* pUnkOuter)
{
	*lplpDDPalette = (LPDIRECTDRAWPALETTE) &IDDPal;
	return ddp_SetEntries( 0, 0, 0, 0, lpColorTable );
}

HRESULT __stdcall DirectDrawCreate( GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter )
{
	*lplpDD = (LPDIRECTDRAW) &IDDraw;
	return 0;
}

HRESULT __stdcall dd_SetCooperativeLevel( void* This, HWND hWnd, DWORD dwFlags )
{ 
	hwnd_main = hWnd;

	// the window size is the original desktop resolution...
	// which is obnoxious when not running in fullscreen.
	SetWindowPos( hWnd, HWND_TOP, 0, 0, width, height, SWP_NOOWNERZORDER | SWP_NOZORDER );

	return 0; 
}

HRESULT __stdcall dds_SetPalette( void* This, LPDIRECTDRAWPALETTE lpDDPalette )
{
	return 0;
}

HRESULT __stdcall ddp_GetEntries( void* This, DWORD dwFlags, DWORD dwBase, DWORD dwNumEntries, LPPALETTEENTRY lpEntries )
{ // can be ignored... because screen not in 8-bit mode
	return 0; 
}

HRESULT __stdcall dd_GetVerticalBlankStatus( void* This, BOOL *lpbIsInVB )
{ // ... can't get with GDI? used for movies.
	return DDERR_UNSUPPORTED;
}

HRESULT __stdcall dd_WaitForVerticalBlank( void* This, DWORD dwFlags, HANDLE hEvent)
{ 
	return DDERR_UNSUPPORTED;
}

ULONG __stdcall iunknown_Release( void* This )
{
	return 0; 
}

const DWORD dd_vtbl[] = {
	0, //QueryInterface,               // 0x00
	0, //AddRef,                       // 0x04
	(DWORD) iunknown_Release,          // 0x08
	0, //Compact,                      // 0x0C
	0, //CreateClipper,                // 0x10
	(DWORD) dd_CreatePalette,          // 0x14
	(DWORD) dd_CreateSurface,          // 0x18
	0, //DuplicateSurface,             // 0x1C
	0, //EnumDisplayModes,             // 0x20
	0, //EnumSurfaces,                 // 0x24
	0, //FlipToGDISurface,             // 0x28
	0, //GetCaps,                      // 0x2C
	0, //GetDisplayMode,               // 0x30
	0, //GetFourCCCodes,               // 0x34
	0, //GetGDISurface,                // 0x38
	0, //GetMonitorFrequency,          // 0x3C
	0, //GetScanLine,                  // 0x40
	(DWORD) dd_GetVerticalBlankStatus, // 0x44
	0, //Initialize,                   // 0x48
	0, //RestoreDisplayMode,           // 0x4C
	(DWORD) dd_SetCooperativeLevel,    // 0x50
	(DWORD) dd_SetDisplayMode,         // 0x54
	(DWORD) dd_WaitForVerticalBlank,   // 0x58
};

const DWORD dds_vtbl[] = {
	0, //QueryInterface,             // 0x00
	0, //AddRef,                     // 0x04
	(DWORD) iunknown_Release,        // 0x08
	0, //AddAttachedSurface,         // 0x0C
	0, //AddOverlayDirtyRect,        // 0x10
	0, //Blt,                        // 0x14
	0, //BltBatch,                   // 0x18
	0, //BltFast,                    // 0x1C
	0, //DeleteAttachedSurface,      // 0x20
	0, //EnumAttachedSurfaces,       // 0x24
	0, //EnumOverlayZOrders,         // 0x28
	0, //Flip,                       // 0x2C
	0, //GetAttachedSurface,         // 0x30
	0, //GetBltStatus,               // 0x34
	0, //GetCaps,                    // 0x38
	0, //GetClipper,                 // 0x3C
	0, //GetColorKey,                // 0x40
	0, //GetDC,                      // 0x44
	0, //GetFlipStatus,              // 0x48
	0, //GetOverlayPosition,         // 0x4C
	0, //GetPalette,                 // 0x50
	0, //GetPixelFormat,             // 0x54
	0, //GetSurfaceDesc,             // 0x58
	0, //Initialize,                 // 0x5C
	0, //IsLost,                     // 0x60
	(DWORD) dds_Lock,                // 0x64
	0, //ReleaseDC,                  // 0x68
	0, //Restore,                    // 0x6C
	0, //SetClipper,                 // 0x70
	0, //SetColorKey,                // 0x74
	0, //SetOverlayPosition,         // 0x78
	(DWORD) dds_SetPalette,          // 0x7C
	(DWORD) dds_Unlock,              // 0x80
	0, //UpdateOverlay,              // 0x84
	0, //UpdateOverlayDisplay,       // 0x88
	0  //UpdateOverlayZOrder,        // 0x8C
};

const DWORD ddp_vtbl[] = { 
	0, //QueryInterface,      // 0x00
	0, //AddRef,              // 0x04
	(DWORD) iunknown_Release, // 0x08
	0, //GetCaps,             // 0x0C
	(DWORD) ddp_GetEntries,   // 0x10
	0, //Initialize,          // 0x14
	(DWORD) ddp_SetEntries    // 0x18
};
