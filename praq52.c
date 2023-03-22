/*
	Filename:     praq52.c
	Description:  PPP style or LZP with variable-length encoding of MTF (SR) codes.
	Written by:   Gerald R. Tamayo, (July 2009, 1/12/2010, 4/14/2010, 9/2/2017, 01/15/2022, 8/22/2022)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   /* C99 */
#include <time.h>
#include "gtbitio2.c"
#include "ucodes2.c"
#include "mtf.c"

/* Bitsize of the first N (i.e., 1<<BSIZE) high-ranking 
symbols, output codesize = 1+BSIZE */
#define BSIZE        3

#define EOF_VLC    256

#define WBITS       20
#define WSIZE       (1<<WBITS)
#define WMASK       (WSIZE-1)

/* PPP_BLOCKBITS must be >= 3 (multiple of 8 bytes blocksize), but <= WBITS */
#define PPP_BLOCKBITS  15
#define PPP_BLOCKSIZE  (1<<PPP_BLOCKBITS)

enum {
	/* modes */
	COMPRESS,
	DECOMPRESS,
};

enum {
	/* codes */
	mPPP = 1,
	mVLCODE
};

typedef struct {
	char alg[8];
	int64_t ppp_nblocks;
	int ppp_lastblocksize;
	int mcode;
} file_stamp;

unsigned char win_buf[ WSIZE ];   /* the prediction buffer or "GuessTable". */
unsigned char pattern[ WSIZE ];   /* the "look-ahead" buffer. */
unsigned char bbuf[PPP_BLOCKSIZE], cbuf[PPP_BLOCKSIZE];
int64_t ppp_nblocks;
int ppp_lastblocksize;
int mcode = 0;

void copyright( void );
void   compress_LZP( unsigned char w[], unsigned char p[] );
void decompress_LZP( unsigned char w[] );

void   compress_VLC( unsigned char w[], unsigned char p[] );
void decompress_VLC( unsigned char w[] );

void usage( void )
{
	fprintf(stderr, "\n Usage: praq52 c[1|2]|d infile outfile\n"
		"\n Commands:\n  c1 = PPP (raw byte output) \n  c2 = MTF coding\n  d  = decoding.\n"
	);
	copyright();
	exit(0);
}

int main( int argc, char *argv[] )
{
	float ratio = 0.0;
	int mode = -1;
	file_stamp fstamp;
	char *cmd = NULL;
	
	clock_t start_time = clock();
	
	if ( argc != 4 ) usage();
	init_buffer_sizes( (1<<15) );
	
	cmd = argv[1];
	while ( cmd ){
		switch( *cmd ) {
			case 'c': if ( mode == -1 ) mode = COMPRESS; else usage(); cmd++; break;
			case 'd':
				if ( mode == -1 ) mode = DECOMPRESS; else usage();
				if ( *(cmd+1) != 0 ) usage(); cmd++; break;
			case '1':
				if ( mode == -1 || mode == DECOMPRESS || mcode ) usage();
				mcode = mPPP; cmd++; break;
			case '2':
				if ( mode == -1 || mode == DECOMPRESS || mcode ) usage();
				mcode = mVLCODE; cmd++; break;
			case 0: cmd = NULL; if ( mcode == 0 ) mcode = mPPP; break;
			default : usage();
		}
	}
	
	if ( (gIN=fopen( argv[2], "rb" )) == NULL ) {
		fprintf(stderr, "\nError opening input file.");
		return 0;
	}
	if ( (pOUT=fopen( argv[3], "wb" )) == NULL ) {
		fprintf(stderr, "\nError opening output file.");
		return 0;
	}
	init_put_buffer();
	
	/* initialize prediction buffer to all zero (0) values. */
	memset( win_buf, 0, WSIZE );
	alloc_mtf(256);
	
	if ( mode == COMPRESS ){
		/* Write the FILE STAMP. */
		strcpy( fstamp.alg, "PRAQ52" );
		fstamp.mcode = mcode;
		fwrite( &fstamp, sizeof(file_stamp), 1, pOUT );
		nbytes_out = sizeof(file_stamp);
		
		fprintf(stderr, "\n Encoding [ %s to %s ] ...", 
			argv[2], argv[3] );
		if ( mcode == mPPP ) compress_LZP( win_buf, pattern );
		else compress_VLC( win_buf, pattern );
	}
	else if ( mode == DECOMPRESS ){
		fread( &fstamp, sizeof(file_stamp), 1, gIN );
		mcode = fstamp.mcode;
		if ( mcode == mPPP ){
			ppp_lastblocksize = fstamp.ppp_lastblocksize;
			ppp_nblocks = fstamp.ppp_nblocks;
		}
		init_get_buffer();
		nbytes_read = sizeof(file_stamp);
		
		fprintf(stderr, "\n Decoding...");
		if ( mcode == mPPP ) decompress_LZP( win_buf );
		else decompress_VLC( win_buf );
		free_get_buffer();
	}
	flush_put_buffer();
	nbytes_read = get_nbytes_read();
	
	if ( mcode == mPPP && mode == COMPRESS ) {
		rewind( pOUT );
		fstamp.ppp_nblocks = ppp_nblocks;
		fstamp.ppp_lastblocksize = ppp_lastblocksize;
		fwrite( &fstamp, sizeof(file_stamp), 1, pOUT );
	}
	
	fprintf(stderr, "done.\n  %s (%lld) -> %s (%lld)", 
		argv[2], nbytes_read, argv[3], nbytes_out);	
	if ( mode == COMPRESS ) {
		ratio = (((float) nbytes_read - (float) nbytes_out) /
			(float) nbytes_read ) * (float) 100;
		fprintf(stderr, "\n Compression ratio: %3.2f %%", ratio );
	}
	fprintf(stderr, " in %3.2f secs.\n", (double) (clock()-start_time) / CLOCKS_PER_SEC );
	
	free_put_buffer();
	free_mtf_table();
	if ( gIN ) fclose( gIN );
	if ( pOUT ) fclose( pOUT );
	
	return 0;
}

void copyright( void )
{
	fprintf(stderr, "\n Written by: Gerald R. Tamayo (c) 2010-2022\n");
}

/* PPP style, a simple preprocessor. */
void compress_LZP( unsigned char w[], unsigned char p[] )
{
	int b = 0, c, n, i, nread, prev=0;  /* prev = context hash */
	
	ppp_nblocks = 0;
	ppp_lastblocksize = 0;
	while ( (nread=fread(p, 1, PPP_BLOCKSIZE, gIN)) ){
		n = 0;
		while ( n < nread ) {
			if ( w[prev] == (c=p[n]) ){  /* Guess/prediction correct */
				put_ONE();
			}
			else {
				put_ZERO();
				w[prev] = c;
				cbuf[b++] = c;  /* record mismatched byte */
			}
			prev = ((prev<<5)+c) & WMASK;  /* update hash */
			n++;
		}
		nbytes_read += nread;
		
		/* write mismatched bytes. */
		if ( nread == PPP_BLOCKSIZE ){
			for ( i = 0; i < b; i++  ) {
				pfputc( cbuf[i] );
			}
			b = 0;
			ppp_nblocks++;
		}
		else if ( nread < PPP_BLOCKSIZE ){ /* last blocksize mismatched bytes */
			/* tricky bits in current *pbuf. */
			if ( p_cnt > 0 && p_cnt < 8 ){
				p_cnt = 7;       /* force byte boundary. */
				advance_buf();   /* writes *pbuf */
			}
			/* write mismatched bytes. */
			for ( i = 0; i < b; i++  ) {
				pfputc( cbuf[i] );
			}
			ppp_lastblocksize = nread;
		}
	}
}

void decompress_LZP( unsigned char w[] )
{
    int c = 0, i = 0, prev = 0;  /* prev = context hash */
	
	if ( ppp_nblocks > 0 ) while ( ppp_nblocks-- ) {
		/* get the block of bits */
		for ( i = 0; i < PPP_BLOCKSIZE; i++ ){
			if ( get_bit() ) bbuf[i] = 1;
			else bbuf[i] = 0;
		}
		
		/* Output bytes. */
		for ( i = 0; i < PPP_BLOCKSIZE; i++ ){
			if ( bbuf[i] == 1 ){
				pfputc( c=w[prev] );
			}
			else {
				pfputc( c=gfgetc() );
				w[prev] = c;
			}
			prev = ((prev<<5)+c) & WMASK;
		}
	}
	
	/* last block */
	if ( ppp_lastblocksize > 0 ) {
		/* get the block of bits */
		for ( i = 0; i < ppp_lastblocksize; i++ ){
			if ( get_bit() ) bbuf[i] = 1;
			else bbuf[i] = 0;
		}
		if ( g_cnt > 0 && g_cnt < 8 ) {
			/* tricky bits. advance gbuf. */
			g_cnt = 7;
			advance_gbuf();
		}
		
		/* Output bytes. */
		for ( i = 0; i < ppp_lastblocksize; i++ ){
			if ( bbuf[i] == 1 ){
				pfputc( c=w[prev] );
			}
			else {
				pfputc( c=gfgetc() );
				w[prev] = c;
			}
			prev = ((prev<<5)+c) & WMASK;
		}
	}
}

void compress_VLC( unsigned char w[], unsigned char p[] )
{
	int c, blen=0, n, rank=0, nread, prev=0;  /* prev = context hash */
	
	while ( (nread=fread(p, 1, WSIZE, gIN)) ){
		n = 0;
		while ( n < nread ) {
			if ( w[prev] == (c=p[n]) ){  /* Guess/prediction correct */
				blen++;
				if ( ++table[c].f >= table[rank].f ) rank = c;
				/* *rank* is the highest (i.e., index 0) in the MTF list. */
				if ( head->c != rank ) mtf(rank);
			}
			else {
				if ( blen ) {
					put_ONE();
					put_vlcode(--blen, 0);
					blen = 0;
				}
				else put_ZERO();
				put_vlcode( mtf(c), BSIZE );
				/* *rank* jumps from symbol to symbol in the MTF list. */
				if ( !(table[rank].f > ++table[c].f && head->c != c) ) rank = c;
				w[prev] = c;
			}
			n++;
			prev = ((prev<<5)+c) & WMASK;  /* update hash */
		}
		nbytes_read += nread;
	}
	/* flag EOF */
	if ( blen ) {
		put_ONE();
		put_vlcode(--blen, 0);
	}
	else put_ZERO();
	put_vlcode(EOF_VLC, BSIZE);
}

void decompress_VLC( unsigned char w[] )
{
	int blen, c, rank=0, prev = 0;  /* prev = context hash */
	
	do {
		if ( get_bit() ){
			blen = get_vlcode(0)+1;
			do {
				pfputc( c=w[prev] );
				if ( ++table[c].f >= table[rank].f ) rank = c;
				mtf(rank);
				prev = ((prev<<5)+c) & WMASK;
			} while ( --blen );
		}
		if ( (c=get_vlcode(BSIZE)) == EOF_VLC ) return;
		c = get_mtf_c(c);
		if ( !(table[rank].f > ++table[c].f && head->c != c) ) rank = c;
		pfputc( c );
		w[prev] = c;
		prev = ((prev<<5)+c) & WMASK;
	} while ( 1 );
}
