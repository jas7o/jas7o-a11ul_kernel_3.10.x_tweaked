/* Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>

#include <asm/cacheflush.h>
#include <asm/compiler.h>

#include <soc/qcom/scm.h>

#define SCM_ENOMEM		-5
#define SCM_EOPNOTSUPP		-4
#define SCM_EINVAL_ADDR		-3
#define SCM_EINVAL_ARG		-2
#define SCM_ERROR		-1
#define SCM_INTERRUPTED		1

static DEFINE_MUTEX(scm_lock);

#define SCM_BUF_LEN(__cmd_size, __resp_size)	\
	(sizeof(struct scm_command) + sizeof(struct scm_response) + \
		__cmd_size + __resp_size)
struct scm_command {
	u32	len;
	u32	buf_offset;
	u32	resp_hdr_offset;
	u32	id;
	u32	buf[0];
};

struct scm_response {
	u32	len;
	u32	buf_offset;
	u32	is_complete;
};

#ifdef CONFIG_HTC_SCM
struct oem_simlock_unlock_req {
	u32	unlock;
	void *code;
};

struct oem_log_oper_req {
	u32	address;
	u32	size;
	u32	buf_addr;
	u32	buf_len;
	int	revert;
};

struct oem_access_item_req {
	u32	is_write;
	u32	id;
	u32	buf_len;
	void *buf;
};

struct oem_3rd_party_syscall_req {
	u32 id;
	void *buf;
	u32 len;
};
#endif

#ifdef CONFIG_ARM64

#define R0_STR "x0"
#define R1_STR "x1"
#define R2_STR "x2"
#define R3_STR "x3"
#define R4_STR "x4"

#define outer_inv_range(x, y)
#define outer_flush_range(x, y)

#define __cpuc_flush_dcache_area __flush_dcache_area

#else

#define R0_STR "r0"
#define R1_STR "r1"
#define R2_STR "r2"
#define R3_STR "r3"
#define R4_STR "r4"

#endif

static inline struct scm_response *scm_command_to_response(
		const struct scm_command *cmd)
{
	return (void *)cmd + cmd->resp_hdr_offset;
}

static inline void *scm_get_command_buffer(const struct scm_command *cmd)
{
	return (void *)cmd->buf;
}

static inline void *scm_get_response_buffer(const struct scm_response *rsp)
{
	return (void *)rsp + rsp->buf_offset;
}

static int scm_remap_error(int err)
{
	pr_err("scm_call failed with error code %d\n", err);
	switch (err) {
	case SCM_ERROR:
		return -EIO;
	case SCM_EINVAL_ADDR:
	case SCM_EINVAL_ARG:
		return -EINVAL;
	case SCM_EOPNOTSUPP:
		return -EOPNOTSUPP;
	case SCM_ENOMEM:
		return -ENOMEM;
	}
	return -EINVAL;
}

static u32 smc(u32 cmd_addr)
{
	int context_id;
	register u32 r0 asm("r0") = 1;
	register u32 r1 asm("r1") = (uintptr_t)&context_id;
	register u32 r2 asm("r2") = cmd_addr;
	do {
		asm volatile(
			__asmeq("%0", R0_STR)
			__asmeq("%1", R0_STR)
			__asmeq("%2", R1_STR)
			__asmeq("%3", R2_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc	#0\n"
			: "=r" (r0)
			: "r" (r0), "r" (r1), "r" (r2)
			: "r3");
	} while (r0 == SCM_INTERRUPTED);

	return r0;
}

static int __scm_call(const struct scm_command *cmd)
{
	int ret;
	u32 cmd_addr = virt_to_phys(cmd);

	__cpuc_flush_dcache_area((void *)cmd, cmd->len);
	outer_flush_range(cmd_addr, cmd_addr + cmd->len);

	ret = smc(cmd_addr);
	if (ret < 0)
		ret = scm_remap_error(ret);

	return ret;
}

#ifndef CONFIG_ARM64
void scm_inv_range(unsigned long start, unsigned long end)
{
	u32 cacheline_size, ctr;

	asm volatile("mrc p15, 0, %0, c0, c0, 1" : "=r" (ctr));
	cacheline_size = 4 << ((ctr >> 16) & 0xf);

	start = round_down(start, cacheline_size);
	end = round_up(end, cacheline_size);
	outer_inv_range(start, end);
	while (start < end) {
		asm ("mcr p15, 0, %0, c7, c6, 1" : : "r" (start)
		     : "memory");
		start += cacheline_size;
	}
	dsb();
	isb();
}
EXPORT_SYMBOL(scm_inv_range);
#else

void scm_inv_range(unsigned long start, unsigned long end)
{
	dmac_inv_range((void *)start, (void *)end);
}
EXPORT_SYMBOL(scm_inv_range);
#endif


static int scm_call_common(u32 svc_id, u32 cmd_id, const void *cmd_buf,
				size_t cmd_len, void *resp_buf, size_t resp_len,
				struct scm_command *scm_buf,
				size_t scm_buf_length)
{
	int ret;
	struct scm_response *rsp;
	unsigned long start, end;

	scm_buf->len = scm_buf_length;
	scm_buf->buf_offset = offsetof(struct scm_command, buf);
	scm_buf->resp_hdr_offset = scm_buf->buf_offset + cmd_len;
	scm_buf->id = (svc_id << 10) | cmd_id;

	if (cmd_buf)
		memcpy(scm_get_command_buffer(scm_buf), cmd_buf, cmd_len);

	mutex_lock(&scm_lock);
	ret = __scm_call(scm_buf);
	mutex_unlock(&scm_lock);
	if (ret)
		return ret;

	rsp = scm_command_to_response(scm_buf);
	start = (unsigned long)rsp;

	do {
		scm_inv_range(start, start + sizeof(*rsp));
	} while (!rsp->is_complete);

	end = (unsigned long)scm_get_response_buffer(rsp) + resp_len;
	scm_inv_range(start, end);

	if (resp_buf)
		memcpy(resp_buf, scm_get_response_buffer(rsp), resp_len);

	return ret;
}

int scm_call_noalloc(u32 svc_id, u32 cmd_id, const void *cmd_buf,
		size_t cmd_len, void *resp_buf, size_t resp_len,
		void *scm_buf, size_t scm_buf_len)
{
	int ret;
	size_t len = SCM_BUF_LEN(cmd_len, resp_len);

	if (cmd_len > scm_buf_len || resp_len > scm_buf_len ||
	    len > scm_buf_len)
		return -EINVAL;

	if (!IS_ALIGNED((unsigned long)scm_buf, PAGE_SIZE))
		return -EINVAL;

	memset(scm_buf, 0, scm_buf_len);

	ret = scm_call_common(svc_id, cmd_id, cmd_buf, cmd_len, resp_buf,
				resp_len, scm_buf, len);
	return ret;

}

int scm_call(u32 svc_id, u32 cmd_id, const void *cmd_buf, size_t cmd_len,
		void *resp_buf, size_t resp_len)
{
	struct scm_command *cmd;
	int ret;
	size_t len = SCM_BUF_LEN(cmd_len, resp_len);

	if (cmd_len > len || resp_len > len)
		return -EINVAL;

	cmd = kzalloc(PAGE_ALIGN(len), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	ret = scm_call_common(svc_id, cmd_id, cmd_buf, cmd_len, resp_buf,
				resp_len, cmd, len);
	kfree(cmd);
	return ret;
}
EXPORT_SYMBOL(scm_call);

#define SCM_CLASS_REGISTER	(0x2 << 8)
#define SCM_MASK_IRQS		BIT(5)
#define SCM_ATOMIC(svc, cmd, n) (((((svc) << 10)|((cmd) & 0x3ff)) << 12) | \
				SCM_CLASS_REGISTER | \
				SCM_MASK_IRQS | \
				(n & 0xf))

s32 scm_call_atomic1(u32 svc, u32 cmd, u32 arg1)
{
	int context_id;
	register u32 r0 asm("r0") = SCM_ATOMIC(svc, cmd, 1);
	register u32 r1 asm("r1") = (uintptr_t)&context_id;
	register u32 r2 asm("r2") = arg1;

	asm volatile(
		__asmeq("%0", R0_STR)
		__asmeq("%1", R0_STR)
		__asmeq("%2", R1_STR)
		__asmeq("%3", R2_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
		"smc	#0\n"
		: "=r" (r0)
		: "r" (r0), "r" (r1), "r" (r2)
		: "r3");
	return r0;
}
EXPORT_SYMBOL(scm_call_atomic1);

s32 scm_call_atomic2(u32 svc, u32 cmd, u32 arg1, u32 arg2)
{
	int context_id;
	register u32 r0 asm("r0") = SCM_ATOMIC(svc, cmd, 2);
	register u32 r1 asm("r1") = (uintptr_t)&context_id;
	register u32 r2 asm("r2") = arg1;
	register u32 r3 asm("r3") = arg2;

	asm volatile(
		__asmeq("%0", R0_STR)
		__asmeq("%1", R0_STR)
		__asmeq("%2", R1_STR)
		__asmeq("%3", R2_STR)
		__asmeq("%4", R3_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
		"smc	#0\n"
		: "=r" (r0)
		: "r" (r0), "r" (r1), "r" (r2), "r" (r3));
	return r0;
}
EXPORT_SYMBOL(scm_call_atomic2);

s32 scm_call_atomic3(u32 svc, u32 cmd, u32 arg1, u32 arg2, u32 arg3)
{
	int context_id;
	register u32 r0 asm("r0") = SCM_ATOMIC(svc, cmd, 3);
	register u32 r1 asm("r1") = (uintptr_t)&context_id;
	register u32 r2 asm("r2") = arg1;
	register u32 r3 asm("r3") = arg2;
	register u32 r4 asm("r4") = arg3;

	asm volatile(
		__asmeq("%0", R0_STR)
		__asmeq("%1", R0_STR)
		__asmeq("%2", R1_STR)
		__asmeq("%3", R2_STR)
		__asmeq("%4", R3_STR)
		__asmeq("%5", R4_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
		"smc	#0\n"
		: "=r" (r0)
		: "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4));
	return r0;
}
EXPORT_SYMBOL(scm_call_atomic3);

s32 scm_call_atomic4_3(u32 svc, u32 cmd, u32 arg1, u32 arg2,
		u32 arg3, u32 arg4, u32 *ret1, u32 *ret2)
{
	int ret;
	int context_id;
	register u32 r0 asm("r0") = SCM_ATOMIC(svc, cmd, 4);
	register u32 r1 asm("r1") = (uintptr_t)&context_id;
	register u32 r2 asm("r2") = arg1;
	register u32 r3 asm("r3") = arg2;
	register u32 r4 asm("r4") = arg3;
	register u32 r5 asm("r5") = arg4;

	asm volatile(
		__asmeq("%0", R0_STR)
		__asmeq("%1", R1_STR)
		__asmeq("%2", R2_STR)
		__asmeq("%3", R0_STR)
		__asmeq("%4", R1_STR)
		__asmeq("%5", R2_STR)
		__asmeq("%6", R3_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
		"smc	#0\n"
		: "=r" (r0), "=r" (r1), "=r" (r2)
		: "r" (r0), "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r5));
	ret = r0;
	if (ret1)
		*ret1 = r1;
	if (ret2)
		*ret2 = r2;
	return r0;
}
EXPORT_SYMBOL(scm_call_atomic4_3);

u32 scm_get_version(void)
{
	int context_id;
	static u32 version = -1;
	register u32 r0 asm("r0");
	register u32 r1 asm("r1");

	if (version != -1)
		return version;

	mutex_lock(&scm_lock);

	r0 = 0x1 << 8;
	r1 = (uintptr_t)&context_id;
	do {
		asm volatile(
			__asmeq("%0", R0_STR)
			__asmeq("%1", R1_STR)
			__asmeq("%2", R0_STR)
			__asmeq("%3", R1_STR)
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc	#0\n"
			: "=r" (r0), "=r" (r1)
			: "r" (r0), "r" (r1)
			: "r2", "r3");
	} while (r0 == SCM_INTERRUPTED);

	version = r1;
	mutex_unlock(&scm_lock);

	return version;
}
EXPORT_SYMBOL(scm_get_version);

#ifdef CONFIG_HTC_SCM
void scm_flush_range(unsigned long start, unsigned long end)
{
	u32 buf_addr, len;

	if (end <= start)
		return;

	buf_addr = virt_to_phys((void *)start);
	len = end - start;

	__cpuc_flush_dcache_area((void *)start, len);
	outer_flush_range(buf_addr, buf_addr + len);
}
EXPORT_SYMBOL(scm_flush_range);

int secure_read_simlock_mask(void)
{
	int ret;
	u32 dummy;

	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_READ_SIMLOCK_MASK,
			&dummy, sizeof(dummy), NULL, 0);

	pr_info("TZ_HTC_SVC_READ_SIMLOCK_MASK ret = %d\n", ret);
	if (ret > 0)
		ret &= 0x1F;
	pr_info("TZ_HTC_SVC_READ_SIMLOCK_MASK modified ret = %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(secure_read_simlock_mask);

int secure_simlock_unlock(unsigned int unlock, unsigned char *code)
{
	int ret;
	struct oem_simlock_unlock_req req;

	req.unlock = unlock;
	req.code = (void *)virt_to_phys(code);

	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_SIMLOCK_UNLOCK,
			&req, sizeof(req), NULL, 0);

	pr_info("TZ_HTC_SVC_SIMLOCK_UNLOCK ret = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(secure_simlock_unlock);

int secure_get_security_level(void)
{
	int ret;
	u32 dummy;

	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_GET_SECURITY_LEVEL,
			&dummy, sizeof(dummy), NULL, 0);

	pr_info("TZ_HTC_SVC_GET_SECURITY_LEVEL ret = %d\n", ret);
	if (ret > 0)
		ret &= 0x0F;
	pr_info("TZ_HTC_SVC_GET_SECURITY_LEVEL modified ret = %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(secure_get_security_level);

int secure_memprot(void)
{
	int ret;
	u32 dummy;

	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_MEMPROT,
			&dummy, sizeof(dummy), NULL, 0);

	pr_info("TZ_HTC_SVC_MEMPROT ret = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(secure_memprot);

int secure_log_operation(unsigned int address, unsigned int size,
		unsigned int buf_addr, unsigned buf_len, int revert)
{
	int ret;
	struct oem_log_oper_req req;
	req.address = address;
	req.size = size;
	req.buf_addr = buf_addr;
	req.buf_len = buf_len;
	req.revert = revert;
	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_LOG_OPERATOR,
			&req, sizeof(req), NULL, 0);
	pr_info("TZ_HTC_SVC_LOG_OPERATOR ret = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL(secure_log_operation);

int secure_access_item(unsigned int is_write, unsigned int id, unsigned int buf_len, unsigned char *buf)
{
	int ret;
	struct oem_access_item_req req;

	req.is_write = is_write;
	req.id = id;
	req.buf_len = buf_len;
	req.buf = (void *)virt_to_phys(buf);

	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_ACCESS_ITEM,
			&req, sizeof(req), NULL, 0);

	pr_info("TZ_HTC_SVC_ACCESS_ITEM id %d ret = %d\n", id, ret);
	return ret;
}

#if 0
int scm_pas_enable_dx_bw(void);
void scm_pas_disable_dx_bw(void);

int secure_3rd_party_syscall(unsigned int id, unsigned char *buf, int len)
{
	int ret;
	int bus_ret;
	struct oem_3rd_party_syscall_req req;
	unsigned long start, end;

	req.id = id;
	req.len = len;
	req.buf = (void *)virt_to_phys(buf);

	bus_ret = scm_pas_enable_dx_bw();
	pet_watchdog();
	ret = scm_call(SCM_SVC_OEM, TZ_HTC_SVC_3RD_PARTY,
			&req, sizeof(req), NULL, 0);
	start = (unsigned long)buf;
	end = start + len;
	scm_inv_range(start, end);
	if (!bus_ret)
		scm_pas_disable_dx_bw();

	return ret;
}
#endif
#endif

#define IS_CALL_AVAIL_CMD	1
int scm_is_call_available(u32 svc_id, u32 cmd_id)
{
	int ret;
	u32 svc_cmd = (svc_id << 10) | cmd_id;
	u32 ret_val = 0;

	ret = scm_call(SCM_SVC_INFO, IS_CALL_AVAIL_CMD, &svc_cmd,
			sizeof(svc_cmd), &ret_val, sizeof(ret_val));
	if (ret)
		return ret;

	return ret_val;
}
EXPORT_SYMBOL(scm_is_call_available);

#define GET_FEAT_VERSION_CMD	3
int scm_get_feat_version(u32 feat)
{
	if (scm_is_call_available(SCM_SVC_INFO, GET_FEAT_VERSION_CMD)) {
		u32 version;
		if (!scm_call(SCM_SVC_INFO, GET_FEAT_VERSION_CMD, &feat,
				sizeof(feat), &version, sizeof(version)))
			return version;
	}
	return 0;
}
EXPORT_SYMBOL(scm_get_feat_version);

