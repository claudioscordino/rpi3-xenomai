/*
 * Copyright (C) 2009 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */
#ifndef _LIB_COBALT_UMM_H
#define _LIB_COBALT_UMM_H

#include <linux/types.h>

void cobalt_init_umm(__u32 vdso_offset);

void cobalt_unmap_umm(void);

struct xnvdso;

extern struct xnvdso *cobalt_vdso;

#endif /* _LIB_COBALT_UMM_H */
