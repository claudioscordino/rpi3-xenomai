/* Copyright (C) 2009 Red Hat, Inc.
 *
 * See ../COPYING for licensing terms.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/mmu_context.h>
#include <linux/export.h>
#include <linux/ipipe.h>

#include <asm/mmu_context.h>

/*
 * use_mm
 *	Makes the calling kernel thread take on the specified
 *	mm context.
 *	(Note: this routine is intended to be called only
 *	from a kernel thread context)
 */
void use_mm(struct mm_struct *mm)
{
	struct mm_struct *active_mm;
	struct task_struct *tsk = current;
	unsigned long flags;

	task_lock(tsk);
	active_mm = tsk->active_mm;
 	ipipe_mm_switch_protect(flags);
	if (active_mm != mm) {
		atomic_inc(&mm->mm_count);
		tsk->active_mm = mm;
	}
	tsk->mm = mm;
#ifdef CONFIG_IPIPE
	switch_mm_irqs_off(active_mm, mm, tsk);
#else
	switch_mm(active_mm, mm, tsk);
#endif
 	ipipe_mm_switch_unprotect(flags);
	task_unlock(tsk);
#ifdef finish_arch_post_lock_switch
	finish_arch_post_lock_switch();
#endif

	if (active_mm != mm)
		mmdrop(active_mm);
}
EXPORT_SYMBOL_GPL(use_mm);

/*
 * unuse_mm
 *	Reverses the effect of use_mm, i.e. releases the
 *	specified mm context which was earlier taken on
 *	by the calling kernel thread
 *	(Note: this routine is intended to be called only
 *	from a kernel thread context)
 */
void unuse_mm(struct mm_struct *mm)
{
	struct task_struct *tsk = current;

	task_lock(tsk);
	sync_mm_rss(mm);
	tsk->mm = NULL;
	/* active_mm is still 'mm' */
	enter_lazy_tlb(mm, tsk);
	task_unlock(tsk);
}
EXPORT_SYMBOL_GPL(unuse_mm);
