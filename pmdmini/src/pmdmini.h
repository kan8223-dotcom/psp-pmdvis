#ifdef __cplusplus
extern "C" {
#endif

#ifndef __PMDMINI_H__
#define __PMDMINI_H__


void pmd_init( char *pcmdir );
void pmd_setrate( int freq );
int pmd_is_pmd( const char *file );
int pmd_play ( char *arg[] , char *pcmdir );
int pmd_play_mem( unsigned char *data, int size );
int pmd_length_sec ( void );
int pmd_loop_sec ( void );
void pmd_renderer ( short *buf , int len );
void pmd_stop ( void );
void pmd_reset_opna ( void );
void pmd_get_title( char *dest );
void pmd_get_compo( char *dest );

int pmd_get_tracks( void );
void pmd_get_current_notes ( int *notes , int len );
int pmd_getloopcount ( void );
unsigned int pmd_get_opna_sample_rate( void );

/* リズム音源ダンプカウンタ取得 (BD,SD,CYM,HH,TOM,RIM の6要素) */
void pmd_get_rhythm_dumps( int *dumps );

/* ME向け: ビジュアライズデータをuncachedバッファに書き出す
 * vis_out[0..5]=rshot, vis_out[6..11]=rdump, vis_out[12..22]=notes */
void pmd_fill_vis_data( volatile unsigned int *vis_out );

/* ME向け: FMPビジュアライザ用データをuncachedバッファに書き出す
 * fmp_out[0..10]=voicenum, [11..21]=volume, [22..32]=detune, [33..43]=fnum */
void pmd_fill_fmp_data( volatile unsigned int *fmp_out );


/* pmdwin2 API — セカンドインスタンス (デュアルCPU WAVエクスポート用) */
void pmd2_setrate( int freq );
void pmd2_play_mem( unsigned char *data, int size );
void pmd2_renderer( short *buf, int len );
void pmd2_stop( void );
int pmd2_getloopcount( void );


#endif

#ifdef __cplusplus
}
#endif
