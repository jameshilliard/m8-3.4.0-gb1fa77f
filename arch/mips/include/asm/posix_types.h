/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996, 97, 98, 99, 2000 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 */
#ifndef _ASM_POSIX_TYPES_H
#define _ASM_POSIX_TYPES_H

#include <asm/sgidefs.h>


#if (_MIPS_SZLONG == 64)
typedef unsigned int	__kernel_nlink_t;
#define __kernel_nlink_t __kernel_nlink_t
#endif

typedef long		__kernel_daddr_t;
#define __kernel_daddr_t __kernel_daddr_t

#if (_MIPS_SZLONG == 32)
typedef struct {
	long	val[2];
} __kernel_fsid_t;
#define __kernel_fsid_t __kernel_fsid_t
#endif

#include <asm-generic/posix_types.h>

#endif 
