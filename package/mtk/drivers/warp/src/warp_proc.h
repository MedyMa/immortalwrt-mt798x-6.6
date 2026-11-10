/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __WARP_PROC_H__
#define __WARP_PROC_H__

struct warp_ctrl;
struct warp_entry;
struct wed_entry;
struct wdma_entry;

int wo_exep_proc_init(struct warp_entry *warp);
void wo_exep_proc_exit(struct warp_entry *warp);
int wo_proc_init(struct warp_entry *warp);
void wo_proc_exit(struct warp_entry *warp);
int rxbm_proc_init(struct warp_entry *warp, struct wed_entry *wed);
void rxbm_proc_exit(struct warp_entry *warp, struct wed_entry *wed);
int wdma_entry_proc_init(struct warp_entry *warp, struct wdma_entry *wdma);
void wdma_entry_proc_exit(struct warp_entry *warp, struct wdma_entry *wdma);
int wed_entry_proc_init(struct warp_entry *warp, struct wed_entry *wed);
void wed_entry_proc_exit(struct warp_entry *warp, struct wed_entry *wed);
int warp_entry_proc_init(struct warp_ctrl *ctrl, struct warp_entry *warp);
void warp_entry_proc_exit(struct warp_ctrl *ctrl, struct warp_entry *warp);
void warp_entry_proc_quiesce(struct warp_entry *warp);
int warp_ctrl_proc_init(struct warp_ctrl *ctrl);
void warp_ctrl_proc_exit(struct warp_ctrl *ctrl);
void warp_ctrl_proc_quiesce(struct warp_ctrl *ctrl);

#endif
