/*
 * Copyright (c) 2003 Matteo Frigo
 * Copyright (c) 2003 Massachusetts Institute of Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: vrank3-transpose.c,v 1.16 2004-04-03 02:31:00 stevenj Exp $ */

/* rank-0, vector-rank-3, square and non-square in-place transposition  */

#include <string.h> /* memcpy */
#include "rdft.h"

typedef struct {
     rdftapply apply;
     int (*applicable)(const problem_rdft *p, planner *plnr,
		       int dim0, int dim1, int dim2, int *nbuf);
     const char *nam;
} transpose_adt;

typedef struct {
     solver super;
     const transpose_adt *adt;
} S;

typedef struct {
     plan_rdft super;
     int n, vl;
     int s0, s1, vs;
     int m;
     int nbuf; /* buffer size */
     int nd, md, d; /* transpose_gcd params */
     int fd;
     const S *slv;
} P;

/**************************************************************************/

/* FIXME: what is the best value for these cutoffs? */

#define CUTOFF 8 /* size below which we do a naive transpose */
#define CUTOFF_UGLY 2000 /* size at which naive transpose is UGLY */

/*************************************************************************/
/* some utilities for the solvers */

static int gcd(int a, int b)
{
     int r;
     do {
	  r = a % b;
	  a = b;
	  b = r;
     } while (r != 0);
     
     return a;
}

/*************************************************************************/
/********************* Generic Ntuple transposes *************************/
/*************************************************************************/

/*************************************************************************/
/* Out-of-place transposes: */

/* Transpose A (n x m) to B (m x n), where A and B are stored
   as n x fda and m x fdb arrays, respectively, operating on N-tuples: */
static void rec_transpose_Ntuple(R *A, R *B, int n, int m, int fda, int fdb,
			  int N)
{
     if (n == 1 || m == 1 || (n + m) * N < CUTOFF*2) {
	  switch (N) {
	      case 1: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
			     B[j*fdb + i] = A[i*fda + j];
			}
		   }
		   break;
	      }
	      case 2: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
#if defined(FFTW_SINGLE) && SIZEOF_FLOAT != 0 && SIZEOF_DOUBLE==2*SIZEOF_FLOAT
			     double *Bd = (double *) (B + (j*fdb + i) * 2);
			     double *Ad = (double *) (A + (i*fda + j) * 2);
			     *Bd = *Ad;
#else
			     B[(j*fdb + i) * 2] = A[(i*fda + j) * 2];
			     B[(j*fdb + i) * 2 + 1] = A[(i*fda + j) * 2 + 1];
#endif
			}
		   }
		   break;
	      }
	      default: {
		   int i, j, k;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
			     for (k = 0; k < N; ++k) {
				  B[(j*fdb + i) * N + k]
				       = A[(i*fda + j) * N + k];
			     }
			}
		   }
	      }
	  }
     }
     else if (n > m) {
	  int n2 = n / 2;
	  rec_transpose_Ntuple(A, B, n2, m, fda, fdb, N);
	  rec_transpose_Ntuple(A + n2*N*fda, B + n2*N, n - n2, m, fda, fdb, N);
     }
     else {
	  int m2 = m / 2;
	  rec_transpose_Ntuple(A, B, n, m2, fda, fdb, N);
	  rec_transpose_Ntuple(A + m2*N, B + m2*N*fdb, n, m - m2, fda, fdb, N);
     }
}

/*************************************************************************/
/* In-place transposes of square matrices of N-tuples: */

/* Transpose both A and B, where A is n x m and B is m x n, storing
   the transpose of A in B and the transpose of B in A.  A and B
   are actually stored as n x fda and m x fda arrays. */
static void rec_transpose_swap_Ntuple(R *A, R *B, int n, int m, int fda, int N)
{
     if (n == 1 || m == 1 || (n + m) * N <= CUTOFF*2) {
	  switch (N) {
	      case 1: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
			     R a = A[(i*fda + j)];
			     A[(i*fda + j)] = B[(j*fda + i)];
			     B[(j*fda + i)] = a;
			}
		   }
		   break;
	      }
	      case 2: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
#if defined(FFTW_SINGLE) && SIZEOF_FLOAT != 0 && SIZEOF_DOUBLE==2*SIZEOF_FLOAT
			     double *Bd = (double *) (B + (j*fda + i) * 2);
			     double *Ad = (double *) (A + (i*fda + j) * 2);
			     double ad = *Ad;
			     *Ad = *Bd;
			     *Bd = ad;
#else
			     R a0 = A[(i*fda + j) * 2 + 0];
			     R a1 = A[(i*fda + j) * 2 + 1];
			     A[(i*fda + j) * 2 + 0] = B[(j*fda + i) * 2 + 0];
			     A[(i*fda + j) * 2 + 1] = B[(j*fda + i) * 2 + 1];
			     B[(j*fda + i) * 2 + 0] = a0;
			     B[(j*fda + i) * 2 + 1] = a1;
#endif
			}
		   }
		   break;
	      }
	      default: {
		   int i, j, k;
		   for (i = 0; i < n; ++i) {
			for (j = 0; j < m; ++j) {
			     for (k = 0; k < N; ++k) {
				  R a = A[(i*fda + j) * N + k];
				  A[(i*fda + j) * N + k] = 
				       B[(j*fda + i) * N + k];
				  B[(j*fda + i) * N + k] = a;
			     }
			}
		   }
	      }
	  }
     } else if (n > m) {
	  int n2 = n / 2;
	  rec_transpose_swap_Ntuple(A, B, n2, m, fda, N);
	  rec_transpose_swap_Ntuple(A + n2*N*fda, B + n2*N, n - n2, m, fda, N);
     }
     else {
	  int m2 = m / 2;
	  rec_transpose_swap_Ntuple(A, B, n, m2, fda, N);
	  rec_transpose_swap_Ntuple(A + m2*N, B + m2*N*fda, n, m - m2, fda, N);
     }
}

/* Transpose A, an n x n matrix (stored as n x fda), in-place. */
static void rec_transpose_sq_ip_Ntuple(R *A, int n, int fda, int N)
{
     if (n == 1)
	  return;
     else if (n*N <= CUTOFF) {
	  switch (N) {
	      case 1: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = i + 1; j < n; ++j) {
			     R a = A[(i*fda + j)];
			     A[(i*fda + j)] = A[(j*fda + i)];
			     A[(j*fda + i)] = a;
			}
		   }
		   break;
	      }
	      case 2: {
		   int i, j;
		   for (i = 0; i < n; ++i) {
			for (j = i + 1; j < n; ++j) {
#if defined(FFTW_SINGLE) && SIZEOF_FLOAT != 0 && SIZEOF_DOUBLE==2*SIZEOF_FLOAT
			     double *Bd = (double *) (A + (j*fda + i) * 2);
			     double *Ad = (double *) (A + (i*fda + j) * 2);
			     double ad = *Ad;
			     *Ad = *Bd;
			     *Bd = ad;
#else
			     R a0 = A[(i*fda + j) * 2 + 0];
			     R a1 = A[(i*fda + j) * 2 + 1];
			     A[(i*fda + j) * 2 + 0] = A[(j*fda + i) * 2 + 0];
			     A[(i*fda + j) * 2 + 1] = A[(j*fda + i) * 2 + 1];
			     A[(j*fda + i) * 2 + 0] = a0;
			     A[(j*fda + i) * 2 + 1] = a1;
#endif
			}
		   }
		   break;
	      }
	      default: {
		   int i, j, k;
		   for (i = 0; i < n; ++i) {
			for (j = i + 1; j < n; ++j) {
			     for (k = 0; k < N; ++k) {
				  R a = A[(i*fda + j) * N + k];
				  A[(i*fda + j) * N + k] = 
				       A[(j*fda + i) * N + k];
				  A[(j*fda + i) * N + k] = a;
			     }
			}
		   }
	      }
	  }
     } else {
	  int n2 = n / 2;
	  rec_transpose_sq_ip_Ntuple(A, n2, fda, N);
	  rec_transpose_sq_ip_Ntuple((A + n2*N) + n2*N*fda, n - n2, fda, N);
	  rec_transpose_swap_Ntuple(A + n2*N, A + n2*N*fda, n2, n - n2, fda,N);
     }
}

/*************************************************************************/

/* Convert an n x n square matrix A between "packed" (n x n) and
   "unpacked" (n x fda) formats.  The "padding" elements at the ends
   of the rows are not preserved. */

static void pack(R *A, int n, int fda, int N)
{
     int i;
     for (i = 0; i < n; ++i)
	  memmove(A + (n*N) * i, A + (fda*N) * i, sizeof(R) * (n*N));
}

static void unpack(R *A, int n, int fda, int N)
{
     int i;
     for (i = n-1; i >= 0; --i)
	  memmove(A + (fda*N) * i, A + (n*N) * i, sizeof(R) * (n*N));
}

/*************************************************************************/
/* Cache-oblivious in-place transpose of non-square matrices, based 
   on transposes of blocks given by the gcd of the dimensions.

   This algorithm is related to algorithm V5 from Murray Dow,
   "Transposing a matrix on a vector computer," Parallel Computing 21
   (12), 1997-2005 (1995), with the modification that we use
   cache-oblivious recursive transpose subroutines (and we derived
   it independently). */

/* Transpose the matrix A in-place, where A is an (n*d) x (m*d) matrix
   of N-tuples and buf contains n*m*d*N elements.  

   In general, to transpose a p x q matrix, you should call this
   routine with d = gcd(p, q), n = p/d, and m = q/d. 

   See also transpose_cut, below, if |p-q| * gcd(p,q) < max(p,q). */
static void transpose_gcd(R *A, int n, int m, int d, int N, R *buf)
{
     A(n > 0 && m > 0 && N > 0 && d > 0);
     if (d == 1) {
	  rec_transpose_Ntuple(A, buf, n,m, m,n, N);
	  memcpy(A, buf, m*n*N*sizeof(R));
     }
     else {
	  int i, num_el = n*m*d*N;

	  /* treat as (d x n) x (d' x m) matrix.  (d' = d) */

	  /* First, transpose d x (n x d') x m to d x (d' x n) x m,
	     using the buf matrix.  This consists of d transposes
	     of contiguous n x d' matrices of m-tuples. */
	  if (n > 1) {
	       for (i = 0; i < d; ++i) {
		    rec_transpose_Ntuple(A + i*num_el, buf,
					 n,d, d,n, m*N);
		    memcpy(A + i*num_el, buf, num_el*sizeof(R));
	       }
	  }
	  
	  /* Now, transpose (d x d') x (n x m) to (d' x d) x (n x m), which
	     is a square in-place transpose of n*m-tuples: */
	  rec_transpose_sq_ip_Ntuple(A, d, d, n*m*N);

	  /* Finally, transpose d' x ((d x n) x m) to d' x (m x (d x n)),
	     using the buf matrix.  This consists of d' transposes
	     of contiguous d*n x m matrices. */
	  if (m > 1) {
	       for (i = 0; i < d; ++i) {
		    rec_transpose_Ntuple(A + i*num_el, buf,
					 d*n,m, m,d*n, N);
		    memcpy(A + i*num_el, buf, num_el*sizeof(R));
	       }
	  }
     }
}

/*************************************************************************/
/* Cache-oblivious in-place transpose of non-square matrices, based 
   a square sub-matrix first and then transposing the remainder
   with the help of a buffer.

   This algorithm is related to algorithm V3 from Murray Dow,
   "Transposing a matrix on a vector computer," Parallel Computing 21
   (12), 1997-2005 (1995), with the modification that we use
   cache-oblivious recursive transpose subroutines */

/* Transpose the matrix A in-place, where A is an n x m matrix
   of N-tuples and buf contains min(n,m) * |n-m| * N elements. 

   See also transpose_gcd, above, if |n-m| * gcd(n,m) > max(n,m). */
static void transpose_cut(R *A, int n, int m, int N, R *buf)
{
     A(n > 0 && m > 0 && N > 0);
     if (n > m) {
	  memcpy(buf, A + m*(m*N), (n-m)*(m*N)*sizeof(R));
	  rec_transpose_sq_ip_Ntuple(A, m, m, N);
	  unpack(A, m, n, N);
	  rec_transpose_Ntuple(buf, A + m*N, n-m,m, m,n, N);
     }
     else if (m > n) {
	  rec_transpose_Ntuple(A + n*N, buf, n,m-n, m,n, N);
	  pack(A, n, m, N);
	  rec_transpose_sq_ip_Ntuple(A, n, n, N);
	  memcpy(A + n*(n*N), buf, (m-n)*(n*N)*sizeof(R));
     }
     else /* n == m */
	  rec_transpose_sq_ip_Ntuple(A, n, n, N);
}

/*************************************************************************/
/* In-place transpose routine from TOMS.  This routine is much slower
   than the cache-oblivious algorithm above, but is has the advantage
   of requiring less buffer space for the case of gcd(nx,ny) small. */

/*
 * TOMS Transpose.  Algorithm 513 (Revised version of algorithm 380).
 * 
 * These routines do in-place transposes of arrays.
 * 
 * [ Cate, E.G. and Twigg, D.W., ACM Transactions on Mathematical Software, 
 *   vol. 3, no. 1, 104-110 (1977) ]
 * 
 * C version by Steven G. Johnson (February 1997).
 */

/*
 * "a" is a 1D array of length ny*nx*N which constains the nx x ny
 * matrix of N-tuples to be transposed.  "a" is stored in row-major
 * order (last index varies fastest).  move is a 1D array of length
 * move_size used to store information to speed up the process.  The
 * value move_size=(ny+nx)/2 is recommended.  buf should be an array
 * of length 2*N.
 * 
 */

static void transpose_toms513(R *a, int nx, int ny, int N,
                              char *move, int move_size, R *buf)
{
     int i, im, mn;
     R *b, *c, *d;
     int ncount;
     int k;
     
     /* check arguments and initialize: */
     A(ny > 0 && nx > 0 && N > 0 && move_size > 0);
     
     b = buf;
     
     /* Cate & Twigg have a special case for nx == ny, but we don't
	bother, since we already have special code for this case elsewhere. */

     c = buf + N;
     ncount = 2;		/* always at least 2 fixed points */
     k = (mn = ny * nx) - 1;
     
     for (i = 0; i < move_size; ++i)
	  move[i] = 0;
     
     if (ny >= 3 && nx >= 3)
	  ncount += gcd(ny - 1, nx - 1) - 1;	/* # fixed points */
     
     i = 1;
     im = ny;
     
     while (1) {
	  int i1, i2, i1c, i2c;
	  int kmi;
	  
	  /** Rearrange the elements of a loop
	      and its companion loop: **/
	  
	  i1 = i;
	  kmi = k - i;
	  memcpy(b, &a[N * i1], N * sizeof(R));
	  i1c = kmi;
	  memcpy(c, &a[N * i1c], N * sizeof(R));
	  
	  while (1) {
	       i2 = ny * i1 - k * (i1 / nx);
	       i2c = k - i2;
	       if (i1 < move_size)
		    move[i1] = 1;
	       if (i1c < move_size)
		    move[i1c] = 1;
	       ncount += 2;
	       if (i2 == i)
		    break;
	       if (i2 == kmi) {
		    d = b;
		    b = c;
		    c = d;
		    break;
	       }
	       memcpy(&a[N * i1], &a[N * i2], 
		      N * sizeof(R));
	       memcpy(&a[N * i1c], &a[N * i2c], 
		      N * sizeof(R));
	       i1 = i2;
	       i1c = i2c;
	  }
	  memcpy(&a[N * i1], b, N * sizeof(R));
	  memcpy(&a[N * i1c], c, N * sizeof(R));
	  
	  if (ncount >= mn)
	       break;	/* we've moved all elements */
	  
	  /** Search for loops to rearrange: **/
	  
	  while (1) {
	       int max = k - i;
	       ++i;
	       A(i <= max);
	       im += ny;
	       if (im > k)
		    im -= k;
	       i2 = im;
	       if (i == i2)
		    continue;
	       if (i >= move_size) {
		    while (i2 > i && i2 < max) {
			 i1 = i2;
			 i2 = ny * i1 - k * (i1 / nx);
		    }
		    if (i2 == i)
			 break;
	       } else if (!move[i])
		    break;
	  }
     }
}

/*************************************************************************/
/****************************** Solvers **********************************/
/*************************************************************************/

/* whether we can transpose with one of our routines expecting
   contiguous Ntuples */
static int Ntuple_transposable(const iodim *a, const iodim *b, int vl, int vs)
{
     return (vs == 1 && b->is == vl && a->os == vl &&
	     ((a->n == b->n && a->is == b->os
	       && a->is >= b->n && a->is % vl == 0)
	      || (a->is == b->n * vl && b->os == a->n * vl)));
}

/* check whether a and b correspond to the first and second dimensions
   of a transpose of tuples with vector length = vl, stride = vs. */
static int transposable(const iodim *a, const iodim *b, int vl, int vs)
{
     return ((a->n == b->n && a->os == b->is && a->is == b->os)
             || Ntuple_transposable(a, b, vl, vs));
}

static int pickdim(const tensor *s, int *pdim0, int *pdim1, int *pdim2)
{
     int dim0, dim1;

     for (dim0 = 0; dim0 < s->rnk; ++dim0)
          for (dim1 = 0; dim1 < s->rnk; ++dim1) {
	       int dim2 = 3 - dim0 - dim1;
	       if (dim0 == dim1) continue;
               if ((s->rnk == 2 || s->dims[dim2].is == s->dims[dim2].os)
		   && transposable(s->dims + dim0, s->dims + dim1, 
				   s->rnk == 2 ? 1 : s->dims[dim2].n,
				   s->rnk == 2 ? 1 : s->dims[dim2].is)) {
                    *pdim0 = dim0;
                    *pdim1 = dim1;
		    *pdim2 = dim2;
                    return 1;
               }
	  }
     return 0;
}

/* generic applicability function */
static int applicable(const solver *ego_, const problem *p_, planner *plnr,
		      int *dim0, int *dim1, int *dim2, int *nbuf)
{
     if (RDFTP(p_)) {
          const S *ego = (const S *) ego_;
          const problem_rdft *p = (const problem_rdft *) p_;

          return (1
		  && p->I == p->O /* FIXME: out-of-place transposes too? */
                  && p->sz->rnk == 0
		  && (p->vecsz->rnk == 2 || p->vecsz->rnk == 3)

                  && pickdim(p->vecsz, dim0, dim1, dim2)

		  /* UGLY if vecloop in wrong order for locality */
		  && (!NO_UGLYP(plnr) ||
		      p->vecsz->rnk == 2 ||
		      X(iabs)(p->vecsz->dims[*dim2].is)
		      < X(imax)(X(iabs)(p->vecsz->dims[*dim0].is),
				X(iabs)(p->vecsz->dims[*dim0].os)))

		  /* UGLY if non-square */
		  && (!NO_UGLYP(plnr)
		      || p->vecsz->dims[*dim0].n == p->vecsz->dims[*dim1].n)
		      
                  && ego->adt->applicable(p, plnr, *dim0,*dim1,*dim2,nbuf)

		  /* buffers too big are UGLY */
		  && ((!NO_UGLYP(plnr) && !CONSERVE_MEMORYP(plnr))
		      || *nbuf <= 65536
		      || *nbuf * 8 < X(tensor_sz)(p->vecsz))
	       );
     }
     return 0;
}

static void get_transpose_vec(const problem_rdft *p, int dim2, int *vl,int *vs)
{
     if (p->vecsz->rnk == 2) {
	  *vl = 1; *vs = 1;
     }
     else {
	  *vl = p->vecsz->dims[dim2].n;
	  *vs = p->vecsz->dims[dim2].is; /* == os */
     }  
}


/* use the simple square transpose in the solver for square matrices
   that aren't too big or which have non-unit stride */
static int transpose_simplep(planner *plnr, const problem_rdft *p,
			     int dim0, int dim1, int dim2)
{
     int n = p->vecsz->dims[dim0].n;
     int vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     return (n == p->vecsz->dims[dim1].n
	     && (!NO_UGLYP(plnr)
		 || n * vl < CUTOFF_UGLY 
		 || vs != 1));
}

/*-----------------------------------------------------------------------*/
/* simple triple-loop square transpose of vector-length 1, no buffers */

static void apply_simple(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n;
     int s0 = ego->s0, s1 = ego->s1;
     int i, j;
     A(n == ego->m && ego->vl == 1);
     UNUSED(O);
     for (i = 1; i < n; ++i) {
          for (j = 0; j < i; ++j) {
	       R *p0 = I + i * s0 + j * s1;
	       R *p1 = I + j * s0 + i * s1;
	       R t0 = p0[0];
	       p0[0] = p1[0];
	       p1[0] = t0;
          }
     }
}

static int applicable_simple(const problem_rdft *p, 
			     planner *plnr,
			     int dim0, int dim1, int dim2, int *nbuf)
{
     *nbuf = 0;
     return (p->vecsz->rnk == 2
	     && transpose_simplep(plnr, p, dim0, dim1, dim2)
	  );
}

static const transpose_adt adt_simple =
{
     apply_simple, applicable_simple,
     "rdft-transpose-simple"
};

/*-----------------------------------------------------------------------*/
/* simple triple-loop square transpose of consecutive pairs, no buffers */

static void apply_simple_consecpairs(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n;
     int s0 = ego->s0, s1 = ego->s1;
     int i, j;
     A(n == ego->m && ego->vl == 2 && ego->vs == 1);
     UNUSED(O);
     for (i = 1; i < n; ++i) {
          for (j = 0; j < i; ++j) {
#if defined(FFTW_SINGLE) && SIZEOF_FLOAT != 0 && SIZEOF_DOUBLE==2*SIZEOF_FLOAT
	       double *p0 = (double *) (I + i * s0 + j * s1);
	       double *p1 = (double *) (I + j * s0 + i * s1);
	       double t0 = p0[0];
	       p0[0] = p1[0];
	       p1[0] = t0;
#else
	       R *p0 = I + i * s0 + j * s1;
	       R *p1 = I + j * s0 + i * s1;
	       R t0 = p0[0], t1 = p0[1];
	       p0[0] = p1[0];
	       p0[1] = p1[1];
	       p1[0] = t0;
	       p1[1] = t1;
#endif
          }
     }
}

static int applicable_simple_consecpairs(const problem_rdft *p, planner *plnr,
			   int dim0, int dim1, int dim2, int *nbuf)
{
     if (nbuf) *nbuf = 0;
     return (p->vecsz->rnk == 3
	     && transpose_simplep(plnr, p, dim0, dim1, dim2)
	     && p->vecsz->dims[dim2].n == 2
	     && p->vecsz->dims[dim2].is == 1
	  );
}

static const transpose_adt adt_simple_consecpairs =
{
     apply_simple_consecpairs, applicable_simple_consecpairs,
     "rdft-transpose-simple-consecpairs"
};

/*-----------------------------------------------------------------------*/
/* simple triple-loop square transpose of vectors, no buffers */

static void apply_simple_vec(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n;
     int s0 = ego->s0, s1 = ego->s1;
     int vl = ego->vl, vs = ego->vs;
     int i, j, iv;
     A(n == ego->m);
     UNUSED(O);
     for (i = 1; i < n; ++i) {
          for (j = 0; j < i; ++j) {
	       R *p0 = I + i * s0 + j * s1;
	       R *p1 = I + j * s0 + i * s1;
	       for (iv = 0; iv < vl; ++iv) {
		    R t0 = p0[0];
		    p0[0] = p1[0]; p0 += vs;
		    p1[0] = t0; p1 += vs;
	       }
          }
     }
}

static int applicable_simple_vec(const problem_rdft *p, planner *plnr,
				 int dim0, int dim1, int dim2, int *nbuf)
{
     *nbuf = 0;
     return (p->vecsz->rnk == 3
	     && transpose_simplep(plnr, p, dim0, dim1, dim2)
	     && (!NO_UGLYP(plnr)
		 || !applicable_simple_consecpairs(p, plnr,
						   dim0, dim1, dim2, 0))
	  );
}

static const transpose_adt adt_simple_vec =
{
     apply_simple_vec, applicable_simple_vec,
     "rdft-transpose-simple-vec"
};

/*-----------------------------------------------------------------------*/
/* cache-oblivious square transpose, vector-stride 1 only, handles
   case where fda > n, no buffers */

static void apply_recsq(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n;
     int fd = ego->fd;
     int vl = ego->vl;
     A(ego->vs == 1 && ego->m == n && ego->s1 == vl);
     UNUSED(O);
     rec_transpose_sq_ip_Ntuple(I, n, fd, vl);
}

static int applicable_recsq(const problem_rdft *p, planner *plnr,
			    int dim0, int dim1, int dim2, int *nbuf)
{
     int vl, vs;
     UNUSED(plnr);
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = 0;
     return (p->vecsz->dims[dim0].n == p->vecsz->dims[dim1].n
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static const transpose_adt adt_recsq =
{
     apply_recsq, applicable_recsq,
     "rdft-transpose-recsq"
};

/*-----------------------------------------------------------------------*/
/* cache-oblivious gcd-based non-square transpose, vector-stride 1 only */

static void apply_gcd(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int nd = ego->nd, md = ego->md, d = ego->d;
     int vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == ego->m * vl
       && ego->n == nd * d && ego->m == md * d);
     UNUSED(O);
     transpose_gcd(I, nd, md, d, vl, buf);
     X(ifree)(buf);
}

static int applicable_gcd(const problem_rdft *p, planner *plnr,
			  int dim0, int dim1, int dim2, int *nbuf)
{
     int n = p->vecsz->dims[dim0].n;
     int m = p->vecsz->dims[dim1].n;
     int vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = n * (m / gcd(n, m)) * vl;
     return (!NO_UGLYP(plnr) /* FIXME: not really ugly for large 1d ffts */
	     && n != m
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static const transpose_adt adt_gcd =
{
     apply_gcd, applicable_gcd,
     "rdft-transpose-gcd"
};

/*-----------------------------------------------------------------------*/
/* cache-oblivious cut-based non-square transpose, vector-stride 1 only */

static void apply_cut(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n, m = ego->m;
     int vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == m * vl);
     UNUSED(O);
     transpose_cut(I, n, m, vl, buf);
     X(ifree)(buf);
}

static int applicable_cut(const problem_rdft *p, planner *plnr,
			  int dim0, int dim1, int dim2, int *nbuf)
{
     int n = p->vecsz->dims[dim0].n;
     int m = p->vecsz->dims[dim1].n;
     int vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = X(imin)(n, m) * X(iabs)(n - m) * vl;
     return (!NO_UGLYP(plnr) /* FIXME: not really ugly for large 1d ffts? */
	     && n != m
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static const transpose_adt adt_cut =
{
     apply_cut, applicable_cut,
     "rdft-transpose-cut"
};

/*-----------------------------------------------------------------------*/
/* non-square transpose from TOMS 513, vector-stride 1 only */

static void apply_toms513(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     int n = ego->n, m = ego->m;
     int vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == m * vl);
     UNUSED(O);
     transpose_toms513(I, n, m, vl, (char *) (buf + 2*vl), (n+m)/2, buf);
     X(ifree)(buf);
}

static int applicable_toms513(const problem_rdft *p, planner *plnr,
			   int dim0, int dim1, int dim2, int *nbuf)
{
     int n = p->vecsz->dims[dim0].n;
     int m = p->vecsz->dims[dim1].n;
     int vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = 2*vl 
	  + ((n + m) / 2 * sizeof(char) + sizeof(R) - 1) / sizeof(R);
     return (!NO_UGLYP(plnr)
	     && n != m
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static const transpose_adt adt_toms513 =
{
     apply_toms513, applicable_toms513,
     "rdft-transpose-toms513"
};

/*-----------------------------------------------------------------------*/
/*-----------------------------------------------------------------------*/
/* generic stuff: */

static void print(const plan *ego_, printer *p)
{
     const P *ego = (const P *) ego_;
     p->print(p, "(%s-%dx%d%v)", ego->slv->adt->nam,
	      ego->n, ego->m, ego->vl);
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     const problem_rdft *p;
     int dim0, dim1, dim2, nbuf;
     P *pln;

     static const plan_adt padt = {
	  X(rdft_solve), X(null_awake), print, X(plan_null_destroy)
     };

     if (!applicable(ego_, p_, plnr, &dim0, &dim1, &dim2, &nbuf))
          return (plan *) 0;

     p = (const problem_rdft *) p_;
     pln = MKPLAN_RDFT(P, &padt, ego->adt->apply);

     pln->n = p->vecsz->dims[dim0].n;
     pln->m = p->vecsz->dims[dim1].n;
     pln->s0 = p->vecsz->dims[dim0].is;
     pln->s1 = p->vecsz->dims[dim1].is;
     get_transpose_vec(p, dim2, &pln->vl, &pln->vs);
     pln->nbuf = nbuf;
     pln->d = gcd(pln->n, pln->m);
     pln->nd = pln->n / pln->d;
     pln->md = pln->m / pln->d;
     pln->fd = pln->s0 / pln->vl;
     pln->slv = ego;

     /* pln->vl * (2 loads + 2 stores) * (pln->n \choose 2) 
        (FIXME? underestimate for non-square) */
     X(ops_other)(2 * pln->vl * pln->n * (pln->m - 1), &pln->super.super.ops);

     return &(pln->super.super);
}

static solver *mksolver(const transpose_adt *adt)
{
     static const solver_adt sadt = { mkplan };
     S *slv = MKSOLVER(S, &sadt);
     slv->adt = adt;
     return &(slv->super);
}

void X(rdft_vrank3_transpose_register)(planner *p)
{
     unsigned i;
     static const transpose_adt *const adts[] = {
	  &adt_simple, &adt_simple_consecpairs, &adt_simple_vec,
	  &adt_recsq, &adt_gcd, &adt_cut, &adt_toms513
     };
     for (i = 0; i < sizeof(adts) / sizeof(adts[0]); ++i)
          REGISTER_SOLVER(p, mksolver(adts[i]));
}
