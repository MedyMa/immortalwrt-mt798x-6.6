/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Verbatim pre-fix public SDK/port function, for regression extraction. */
int mtk_npu_tnl_offload(struct npu_tnl_input *tnl_input,
			struct npu_tnl_type *tnl_type,
			struct npu_tnl_params *tnl_params,
			tnl_info_setup_func_t setup_func)
{
	struct npu_tnl_info *tnl_info;
	unsigned long flag;
	int ret = 0;

	if (unlikely(!tnl_params || !setup_func))
		return -EPERM;

	/* prepare tnl_info */
	spin_lock_irqsave(&npu_tnl.tbl_lock, flag);

	tnl_info = mtk_npu_tnl_info_find_no_lock(tnl_type, tnl_params);
	if (IS_ERR(tnl_info) && PTR_ERR(tnl_info) != -ENODEV) {
		/* error */
		ret = PTR_ERR(tnl_info);
		goto err_out;
	} else if (IS_ERR(tnl_info) && PTR_ERR(tnl_info) == -ENODEV) {
		/* not allocate yet */
		tnl_info = mtk_npu_tnl_info_alloc_no_lock(tnl_type);
	}

	if (IS_ERR(tnl_info)) {
		ret = PTR_ERR(tnl_info);
		NPU_DBG("tnl offload alloc tnl_info failed: %d\n", ret);
		goto err_out;
	}

	ret = setup_func(tnl_input, tnl_type, tnl_info, tnl_params);

err_out:
	spin_unlock_irqrestore(&npu_tnl.tbl_lock, flag);

	return ret;
}
