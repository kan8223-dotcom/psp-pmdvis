#include <cstring>
#include <cstdio>

#include "pmdwin/pmdwinimport.h"
#include "pmdmini.h"

int pmd_length = 0;
int pmd_loop = 0;

char pmd_title[1024];
char pmd_compo[1024];
char pmd_file[2048];

OPEN_WORK *pmdwork = NULL;

//
// path splitter
//

static int pmd_split_dir( const char *file , char *dir )
{
	char *p;
	int len = 0;

#ifdef _MSC_VER
	p = std::strrchr ((char*)file, '\\');
#else
	p = std::strrchr ((char*)file, '/');
#endif

	if ( p )
	{
		len = (int)( p + 1 - file );
		std::strncpy ( dir , file , len );
	}
	dir[ len ] = 0;

	return len;
}

//
// 初期化
//

void pmd_init( char *pcmdir )
{
#ifdef _MSC_VER
	char* current_dir = (char*)(".\\");
#else
	char* current_dir = (char*)("./");
#endif
	if (0 != pcmdir[0])
	{
		current_dir = pcmdir;
	}
	pmdwininit( current_dir );
	setpcmrate( SOUND_55K );

	pmdwork = NULL;

	pmd_length = 0;
	pmd_loop = 0;
}

//
//　周波数設定
//

void pmd_setrate( int freq )
{
	setpcmrate( freq );
}

//
// ファイルチェック
//

int pmd_is_pmd( const char *file )
{
	int  size;
	unsigned char header[3];

	FILE *fp;

	fp = std::fopen(file,"rb");

	if (!fp)
		return 0;

	size = (int)std::fread(header,1,3,fp);

	std::fclose(fp);

	if (size != 3)
		return 0;

	if (header[0] > 0x0f)
		return 0;

	if (header[1] != 0x18 && header[1] !=0x1a )
		return 0;

	if (header[2] && header[2] != 0xe6)
		return 0;

	return 1;
}

//
// エラーであれば0以外を返す
//

int pmd_play ( char *argv[] , char *pcmdir )
{
	char dir[2048];
	TCHAR pps_file[1024];
	TCHAR *pps_file_ptr;

	char *path[4];
#ifdef _MSC_VER
	char *current_dir = (char *)(".\\");
#else
	char *current_dir = (char *)"./";
#endif

	char *file = argv[1];
	if ( ! pmd_is_pmd ( file ) )
		return 1;

	std::strcpy( pmd_file , file );

	dir[0] = 0;
	if ( pmd_split_dir( file , dir ) > 0 )
	{
		path[0] = dir;
		path[1] = pcmdir;
		path[2] = (0 != std::strcmp( current_dir, dir )) ? current_dir : nullptr;
		path[3] = nullptr;
	}
	else
	{
		path[0] = current_dir;
		path[1] = pcmdir;
		path[2] = nullptr;
		path[3] = nullptr;
	}
	if (nullptr != path[0]) { printf("path[0] %s\n", path[0]); }
	if (nullptr != path[1]) { printf("path[1] %s\n", path[1]); }
	if (nullptr != path[2]) { printf("path[2] %s\n", path[2]); }
	if (nullptr != path[3]) { printf("path[3] %s\n", path[3]); }

	setpcmdir( path );

	// get song length in sec
	if ( !getlength( pmd_file , &pmd_length , &pmd_loop ) )
	{
		pmd_length = 0;
		pmd_loop = 0;
	}

	pmd_title[0] = 0;
	pmd_compo[0] = 0;
	pps_file[0] = 0;

	music_load( pmd_file );

	pps_file_ptr = getppsfilename( pps_file );

	if (nullptr != pps_file_ptr)
	{
		if ( (nullptr != argv[2]) && (0 == pps_file_ptr[0]) )
		{
			if (nullptr != argv[3])
			{
				if ( (45 /* minus sign */ != argv[3][0]) && (45 /* minus sign */ != argv[3][1]) )
				{
					char *p = nullptr;

#ifdef _MSC_VER
					p = strrchr( argv[3], ':' );
					if ( nullptr == p )
					{
						p = strrchr( argv[3], '\\' );
					}
#else
					p = strrchr( argv[3], '/' );
#endif

					if ( nullptr != p )
					{
						pps_file_ptr = (TCHAR *)argv[3];
						printf("pps_file_ptr %s\n", pps_file_ptr);
						if ( PMDWIN_OK == ppc_load( pps_file_ptr ) )
						{
							printf("PMDWIN_OK == ppc_load( %s )\n", pps_file_ptr);
						}
						else
						{
							printf("PMDWIN_OK != ppc_load( %s )\n", pps_file_ptr);
						}
					}
					else
					{
						if (0 != std::strcmp( current_dir, path[0] ))
						{
							p = std::strcat( pps_file_ptr, path[0] );
						}
						else
						{
							p = std::strcat( pps_file_ptr, current_dir );
						}
						p = std::strcat( pps_file_ptr, argv[3] );
						printf("pps_file_ptr %s\n", pps_file_ptr);

						if ( PMDWIN_OK == ppc_load( pps_file_ptr ) )
						{
							printf("PMDWIN_OK == ppc_load( %s )\n", pps_file_ptr);
						}
						else
						{
							printf("PMDWIN_OK != ppc_load( %s )\n", pps_file_ptr);
						}
					}
				}
			}
		}
	}

	fgetmemo3( pmd_title, pmd_file, 1 );
	fgetmemo3( pmd_compo, pmd_file, 2 );

	setrhythmwithssgeffect( true ); // true == SSG+RHY, false == SSG

	if ( nullptr != pps_file_ptr )
	{
		if ( 0 != pps_file_ptr[0] )
		{
			setppsuse( true ); // PSSDRV FLAG set false at init. true == use PPS, false == do not use PPS
		}
	}

	music_start();

	pmdwork = getopenwork();

	return 0;
}

//
// トラック数
//
int pmd_get_tracks( void )
{
	return NumOfAllPart;
}

//
// 現在のノート
//
void pmd_get_current_notes ( int *notes , int len )
{
	int i = 0;

	for ( i = 0; i < len; i++ ) notes[i] = -1;

	if ( ! pmdwork )
		return;

	for ( i = 0; i < len ; i++ )
	{
		int data = pmdwork->MusPart[i]->onkai;

		if (data == 0xff)
			notes[i] = -1;
		else
			notes[i] = data;
	}
}

int pmd_length_sec ( void )
{
	return pmd_length / 1000;
}

int pmd_loop_sec ( void )
{
	return pmd_loop / 1000;
}

void pmd_renderer ( short *buf , int len )
{
	getpcmdata ( buf , len );
}

int pmd_getloopcount ( void )
{
	return getloopcount();
}

unsigned int pmd_get_opna_sample_rate( void )
{
	return get_opna_sample_rate();
}

void pmd_stop ( void )
{
	music_stop();
	pmdwork = NULL;
}

void pmd_reset_opna ( void )
{
	reset_opna();
}


void pmd_get_title( char *dest )
{
	std::strcpy( dest , pmd_title );
}

void pmd_get_compo( char *dest )
{
	std::strcpy( dest , pmd_compo );
}

void pmd_get_rhythm_dumps( int *dumps )
{
	if ( ! pmdwork ) {
		for (int i = 0; i < 6; i++) dumps[i] = 0;
		return;
	}
	/* rshot (keyon) + rdump (dump) 合算 */
	dumps[0] = pmdwork->rshot_bd  + pmdwork->rdump_bd;
	dumps[1] = pmdwork->rshot_sd  + pmdwork->rdump_sd;
	dumps[2] = pmdwork->rshot_sym + pmdwork->rdump_sym;
	dumps[3] = pmdwork->rshot_hh  + pmdwork->rdump_hh;
	dumps[4] = pmdwork->rshot_tom + pmdwork->rdump_tom;
	dumps[5] = pmdwork->rshot_rim + pmdwork->rdump_rim;
}

void pmd_fill_vis_data( volatile unsigned int *vis_out )
{
	if ( ! pmdwork ) {
		for (int i = 0; i < 23; i++) vis_out[i] = 0;
		return;
	}
	/* rshot[0..5] */
	vis_out[0] = (unsigned int)pmdwork->rshot_bd;
	vis_out[1] = (unsigned int)pmdwork->rshot_sd;
	vis_out[2] = (unsigned int)pmdwork->rshot_sym;
	vis_out[3] = (unsigned int)pmdwork->rshot_hh;
	vis_out[4] = (unsigned int)pmdwork->rshot_tom;
	vis_out[5] = (unsigned int)pmdwork->rshot_rim;
	/* rdump[6..11] */
	vis_out[6]  = (unsigned int)pmdwork->rdump_bd;
	vis_out[7]  = (unsigned int)pmdwork->rdump_sd;
	vis_out[8]  = (unsigned int)pmdwork->rdump_sym;
	vis_out[9]  = (unsigned int)pmdwork->rdump_hh;
	vis_out[10] = (unsigned int)pmdwork->rdump_tom;
	vis_out[11] = (unsigned int)pmdwork->rdump_rim;
	/* notes[12..22] — 11パート */
	int nparts = pmd_get_tracks();
	for (int i = 0; i < 11; i++) {
		if (i < nparts && pmdwork->MusPart[i]) {
			int d = pmdwork->MusPart[i]->onkai;
			vis_out[12 + i] = (d == 0xff) ? 0xFFFFFFFF : (unsigned int)d;
		} else {
			vis_out[12 + i] = 0xFFFFFFFF;
		}
	}
}

void pmd_fill_fmp_data( volatile unsigned int *fmp_out )
{
	if ( ! pmdwork ) {
		for (int i = 0; i < 44; i++) fmp_out[i] = 0;
		return;
	}
	int nparts = pmd_get_tracks();
	for (int i = 0; i < 11; i++) {
		if (i < nparts && pmdwork->MusPart[i]) {
			QQ *q = pmdwork->MusPart[i];
			fmp_out[i]      = (unsigned int)q->voicenum;
			fmp_out[11 + i] = (unsigned int)q->volume;
			fmp_out[22 + i] = (unsigned int)(q->detune & 0xFFFF);
			fmp_out[33 + i] = (unsigned int)q->fnum;
		} else {
			fmp_out[i]      = 0;
			fmp_out[11 + i] = 0;
			fmp_out[22 + i] = 0;
			fmp_out[33 + i] = 0;
		}
	}
}

int pmd_play_mem( unsigned char *data, int size )
{
	music_load2( (uint8_t *)data, (int32_t)size );
	setrhythmwithssgeffect( true );
	music_start();
	pmdwork = getopenwork();
	return 0;
}

/* pmdwin2 API ラッパー */
extern "C" {
extern void setpcmrate2(int rate);
extern int music_load2_inst2(uint8_t *musdata, int size);
extern void music_start2(void);
extern void music_stop2(void);
extern void getpcmdata2(short *buf, int nsamples);
extern int getloopcount2(void);
extern void setrhythmwithssgeffect2(bool value);
}

void pmd2_setrate( int freq ) { setpcmrate2(freq); }
void pmd2_play_mem( unsigned char *data, int size ) {
	music_load2_inst2( (uint8_t *)data, (int32_t)size );
	setrhythmwithssgeffect2( true );
	music_start2();
}
void pmd2_renderer( short *buf, int len ) { getpcmdata2(buf, len); }
void pmd2_stop( void ) { music_stop2(); }
int pmd2_getloopcount( void ) { return getloopcount2(); }

