/* Copyright (c) 2010-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mfd/pmic8901.h>
#include <linux/mfd/pm8xxx/misc.h>
#include <linux/qpnp/power-on.h>

#include <asm/mach-types.h>
#include <asm/cacheflush.h>

#include <mach/msm_iomap.h>
#include <mach/restart.h>
#include <mach/socinfo.h>
#include <mach/irqs.h>
#include <mach/scm.h>
#include "msm_watchdog.h"
#include "timer.h"
#include "wdog_debug.h"

#define WDT0_RST	0x38
#define WDT0_EN		0x40
#define WDT0_BARK_TIME	0x4C
#define WDT0_BITE_TIME	0x5C

#define PSHOLD_CTL_SU (MSM_TLMM_BASE + 0x820)

#define RESTART_REASON_ADDR 0x65C
#define DLOAD_MODE_ADDR     0x0
#define EMERGENCY_DLOAD_MODE_ADDR    0xFE0
#define EMERGENCY_DLOAD_MAGIC1    0x322A4F99
#define EMERGENCY_DLOAD_MAGIC2    0xC67E4350
#define EMERGENCY_DLOAD_MAGIC3    0x77777777

#define SCM_IO_DISABLE_PMIC_ARBITER	1

#ifdef CONFIG_MSM_RESTART_V2
#define use_restart_v2()	1
#else
#define use_restart_v2()	0
#endif

static int restart_mode;
void *restart_reason;

int pmic_reset_irq;
static void __iomem *msm_tmr0_base;

#ifdef CONFIG_MSM_DLOAD_MODE
static int in_panic;
static void *dload_mode_addr;
static bool dload_mode_enabled;
static void *emergency_dload_mode_addr;

/* Download mode master kill-switch */
static int dload_set(const char *val, struct kernel_param *kp);
#ifndef CONFIG_VENDOR_EDIT  
//He Wei@OnLineRD,  2013/08/02, modify for reboot after crash
static int download_mode = 1;
#else      /*CONFIG_VENDOR_EDIT*/
#ifndef RELEASE_DOWNLOAD_MODE_SET
static int download_mode = 1;
#else
static int download_mode = 0;
#endif   /*CONFIG_VENDOR_EDIT*/
#endif
module_param_call(download_mode, dload_set, param_get_int,
			&download_mode, 0644);
static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= panic_prep_restart,
};

#define __LOG_BUF_LEN	(1 << CONFIG_LOG_BUF_SHIFT)
extern char __log_buf[__LOG_BUF_LEN];

extern char * get_serialno(void);

typedef unsigned int	uint32;

#ifdef CONFIG_VENDOR_EDIT
struct boot_shared_imem_cookie_type
{
  /* First 8 bytes are two dload magic numbers */
  uint32 dload_magic_1;
  uint32 dload_magic_2;

  /* Magic number which indicates boot shared imem has been initialized
     and the content is valid.*/ 
  uint32 shared_imem_magic;

  /* Magic number for UEFI ram dump, if this cookie is set along with dload magic numbers,
     we don't enter dload mode but continue to boot. This cookie should only be set by UEFI*/
  uint32 uefi_ram_dump_magic;

  /* Pointer that points to etb ram dump buffer, should only be set by HLOS */
  uint32 etb_buf_addr;

  /* Region where HLOS will write the l2 cache dump buffer start address */
  uint32 *l2_cache_dump_buff_ptr;

  uint32 ddr_training_cookie;

  /* Cookie that will be used to sync with RPM */
  uint32 rpm_sync_cookie;

  /* Abnormal reset cookie used by UEFI */
  uint32 abnormal_reset_occurred;

  /* Reset Status Register */
  uint32 reset_status_register;

  uint32 kernel_log_buff_start;
  uint32 kernel_log_buff_size;

  /* Please add new cookie here, do NOT modify or rearrange the existing cookies*/
};
#endif

#ifdef VENDOR_EDIT
//hefaxi@filesystems, 2015/07/03, add for force dump function
int oem_get_download_mode(void)
{
	return download_mode;
}
#endif

static void set_dload_mode(int on)
{
	if (dload_mode_addr) {
		char *serialno;
		serialno = get_serialno();
		__raw_writel(on ? 0xE47B337D : 0, dload_mode_addr);
		__raw_writel(on ? 0xCE14091A : 0,
		       dload_mode_addr + sizeof(unsigned int));
		#ifdef CONFIG_VENDOR_EDIT
		//Add this to value for dump KMSG.bin
		__raw_writel(on ? (virt_to_phys(__log_buf)) : 0, dload_mode_addr + sizeof(unsigned int) *10 );
		__raw_writel(on ? __LOG_BUF_LEN : 0, dload_mode_addr + sizeof(unsigned int) *11 );
		// #ifdef VENDOR_EDIT
		// neiltsai, 20150812, add for ram dump kernel version
		__raw_writel(on ? (virt_to_phys(linux_banner)): 0, dload_mode_addr + sizeof(unsigned int) *12);
		__raw_writel(on ? (strlen(linux_banner)): 0, dload_mode_addr + sizeof(unsigned int) *13);
		// neil end
                __raw_writel(on ? (virt_to_phys(serialno)): 0, dload_mode_addr + sizeof(unsigned int) *14);
                __raw_writel(on ? (strlen(serialno)): 0, dload_mode_addr + sizeof(unsigned int) *15);
		// #endif
		#endif
		mb();
		dload_mode_enabled = on;
	}
}

#ifndef CONFIG_VENDOR_EDIT
/* OPPO zhanglong modified 2013-08-29 for reboot into quickboot charging */
/* delete this for compiling warning*/
static bool get_dload_mode(void)
{
	return dload_mode_enabled;
}
#endif //CONFIG_VENDOR_EDIT

static void enable_emergency_dload_mode(void)
{
	if (emergency_dload_mode_addr) {
		__raw_writel(EMERGENCY_DLOAD_MAGIC1,
				emergency_dload_mode_addr);
		__raw_writel(EMERGENCY_DLOAD_MAGIC2,
				emergency_dload_mode_addr +
				sizeof(unsigned int));
		__raw_writel(EMERGENCY_DLOAD_MAGIC3,
				emergency_dload_mode_addr +
				(2 * sizeof(unsigned int)));

		/* Need disable the pmic wdt, then the emergency dload mode
		 * will not auto reset. */
		qpnp_pon_wd_config(0);
		mb();
	}
}

static int dload_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = download_mode;

	ret = param_set_int(val, kp);

	if (ret)
		return ret;

	/* If download_mode is not zero or one, ignore. */
	if (download_mode >> 1) {
		download_mode = old_val;
		return -EINVAL;
	}

	set_dload_mode(download_mode);

	return 0;
}
#else
#define set_dload_mode(x) do {} while (0)

static void enable_emergency_dload_mode(void)
{
	printk(KERN_ERR "dload mode is not enabled on target\n");
}

static bool get_dload_mode(void)
{
	return false;
}
#endif

void msm_set_restart_mode(int mode)
{
	restart_mode = mode;
}
EXPORT_SYMBOL(msm_set_restart_mode);

static bool scm_pmic_arbiter_disable_supported;
/*
 * Force the SPMI PMIC arbiter to shutdown so that no more SPMI transactions
 * are sent from the MSM to the PMIC.  This is required in order to avoid an
 * SPMI lockup on certain PMIC chips if PS_HOLD is lowered in the middle of
 * an SPMI transaction.
 */
static void halt_spmi_pmic_arbiter(void)
{
	if (scm_pmic_arbiter_disable_supported) {
		pr_crit("Calling SCM to disable SPMI PMIC arbiter\n");
		scm_call_atomic1(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER, 0);
	}
}

static void __msm_power_off(int lower_pshold)
{
	printk(KERN_CRIT "Powering off the SoC\n");
#ifdef CONFIG_MSM_DLOAD_MODE
	set_dload_mode(0);
#endif
	pm8xxx_reset_pwr_off(0);
	qpnp_pon_system_pwr_off(PON_POWER_OFF_SHUTDOWN);

	if (lower_pshold) {
		if (!use_restart_v2()) {
			__raw_writel(0, PSHOLD_CTL_SU);
		} else {
			halt_spmi_pmic_arbiter();
			__raw_writel(0, MSM_MPM2_PSHOLD_BASE);
		}

		mdelay(10000);
		printk(KERN_ERR "Powering off has failed\n");
	}
	return;
}

static void msm_power_off(void)
{
	/* MSM initiated power off, lower ps_hold */
	__msm_power_off(1);
}

static void cpu_power_off(void *data)
{
	int rc;

	pr_err("PMIC Initiated shutdown %s cpu=%d\n", __func__,
						smp_processor_id());
	if (smp_processor_id() == 0) {
		/*
		 * PMIC initiated power off, do not lower ps_hold, pmic will
		 * shut msm down
		 */
		__msm_power_off(0);

		pet_watchdog();
		pr_err("Calling scm to disable arbiter\n");
		/* call secure manager to disable arbiter and never return */
		rc = scm_call_atomic1(SCM_SVC_PWR,
						SCM_IO_DISABLE_PMIC_ARBITER, 1);

		pr_err("SCM returned even when asked to busy loop rc=%d\n", rc);
		pr_err("waiting on pmic to shut msm down\n");
	}

	preempt_disable();
	while (1)
		;
}

static irqreturn_t resout_irq_handler(int irq, void *dev_id)
{
	pr_warn("%s PMIC Initiated shutdown\n", __func__);
	oops_in_progress = 1;
	smp_call_function_many(cpu_online_mask, cpu_power_off, NULL, 0);
	if (smp_processor_id() == 0)
		cpu_power_off(NULL);
	preempt_disable();
	while (1)
		;
	return IRQ_HANDLED;
}

#ifdef CONFIG_VENDOR_EDIT
/*  2013.07.09 hewei modify begin for restart mode*/
#define FACTORY_MODE	0x77665504
#define WLAN_MODE		0x77665505
#define RF_MODE			0x77665506
#define MOS_MODE		0x77665507
#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500
/*  2013.07.09 hewei modify end for restart mode*/
#endif //CONFIG_VENDOR_EDIT

static void msm_restart_prepare(const char *cmd)
{
#ifdef CONFIG_MSM_DLOAD_MODE
	pr_info("%s : in_panic: %s, restart_mode: %s, download_mode: %s\n",
			__func__, in_panic?"Y":"N", restart_mode?"Y":"N", download_mode?"Y":"N" );
	/* This looks like a normal reboot at this point. */
	set_dload_mode(0);

	/* Write download mode flags if we're panic'ing */
	set_dload_mode(in_panic);

	/* Write download mode flags if restart_mode says so */
	if (restart_mode == RESTART_DLOAD)
		set_dload_mode(1);

	/* Kill download mode if master-kill switch is set */
	if (!download_mode)
		set_dload_mode(0);
#endif

	pm8xxx_reset_pwr_off(1);

#ifndef CONFIG_VENDOR_EDIT
/*  zhanglong modified 2013-08-29 for reboot into quickboot charging */
	/* Hard reset the PMIC unless memory contents must be maintained. */
	if (get_dload_mode() || (cmd != NULL && cmd[0] != '\0'))
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
	else
		qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);
#else
/* We don't do hard reset when reboot. Because we wan't the restart reason
    in the shared memory any way. If a hard reset was done, that will be lost.*/ 
    qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
/*  zhanglong modified 2013-08-29 for reboot into quickboot charging end*/
#endif //CONFIG_VENDOR_EDIT

#ifndef CONFIG_VENDOR_EDIT
/*  2013.07.09 hewei modify begin for restart mode*/
	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			__raw_writel(0x77665500, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			__raw_writel(0x77665502, restart_reason);
		} else if (!strcmp(cmd, "rtc")) {
			__raw_writel(0x77665503, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			code = simple_strtoul(cmd + 4, NULL, 16) & 0xff;
			__raw_writel(0x6f656d00 | code, restart_reason);
		} else if (!strncmp(cmd, "edl", 3)) {
			enable_emergency_dload_mode();
		} else {
			__raw_writel(0x77665501, restart_reason);
		}
	}
#else //CONFIG_VENDOR_EDIT
	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			__raw_writel(FASTBOOT_MODE, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			__raw_writel(RECOVERY_MODE, restart_reason);
		}  else if (!strncmp(cmd, "rf", 2)) {
			__raw_writel(RF_MODE, restart_reason);
		}   else if (!strncmp(cmd, "wlan", 4)) {
			__raw_writel(WLAN_MODE, restart_reason);
		}   else if (!strncmp(cmd, "mos", 3)) {
			__raw_writel(MOS_MODE, restart_reason);
		}   else if (!strncmp(cmd, "ftm", 3)) {
			__raw_writel(FACTORY_MODE, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			code = simple_strtoul(cmd + 4, NULL, 16) & 0xff;
			__raw_writel(0x6f656d00 | code, restart_reason);
		} else if (!strncmp(cmd, "kernel", 6)) {
            __raw_writel(0x7766550a, restart_reason);
        } else if (!strncmp(cmd, "modem", 5)) {
            __raw_writel(0x7766550b, restart_reason);
        } else if (!strncmp(cmd, "android", 7)) {
            __raw_writel(0x7766550c, restart_reason);
		} else if (!strncmp(cmd, "edl", 3)) {
			enable_emergency_dload_mode();
		} else {
			__raw_writel(0x77665501, restart_reason);
			  }
	}else {
		__raw_writel(0x77665501, restart_reason);
	}
/* OPPO 2013.07.09 hewei modify en for restart mode*/
#endif //CONFIG_VENDOR_EDIT

	flush_cache_all();
	outer_flush_all();
}

#ifdef VENDOR_EDIT
/* add by yangrujin@bsp 2015/9/6, flush console to get more log*/
void arm_machine_flush_console(void);
#endif /* VENDOR_EDIT*/

void msm_restart(char mode, const char *cmd)
{
	printk(KERN_NOTICE "Going down for restart now\n");

	msm_restart_prepare(cmd);

#if (defined(CONFIG_MSM_DLOAD_MODE) && defined(VENDOR_EDIT))
/* add by yangrujin@bsp 2015/9/6, flush console to get more log*/
    if(in_panic){
        pr_info("%s : flush console and then delay 1s waiting log printing", __func__);
        arm_machine_flush_console();
        mdelay(1000);
    }/*else{
        pr_info("%s : not in panic. flush console and then delay 1s waiting log printing\n", __func__);
        arm_machine_flush_console();
        mdelay(1000);
    }*/
#endif /* VENDOR_EDIT*/

	if (!use_restart_v2()) {
		__raw_writel(0, msm_tmr0_base + WDT0_EN);
		if (!(machine_is_msm8x60_fusion() ||
		      machine_is_msm8x60_fusn_ffa())) {
			mb();
			 /* Actually reset the chip */
			__raw_writel(0, PSHOLD_CTL_SU);
			mdelay(5000);
			pr_notice("PS_HOLD didn't work, falling back to watchdog\n");
		}

		__raw_writel(1, msm_tmr0_base + WDT0_RST);
		__raw_writel(5*0x31F3, msm_tmr0_base + WDT0_BARK_TIME);
		__raw_writel(0x31F3, msm_tmr0_base + WDT0_BITE_TIME);
		__raw_writel(1, msm_tmr0_base + WDT0_EN);
	} else {
		/* Needed to bypass debug image on some chips */
		msm_disable_wdog_debug();
		halt_spmi_pmic_arbiter();
		__raw_writel(0, MSM_MPM2_PSHOLD_BASE);
	}

	mdelay(10000);
	printk(KERN_ERR "Restarting has failed\n");
}

static int __init msm_pmic_restart_init(void)
{
	int rc;

	if (use_restart_v2())
		return 0;

	if (pmic_reset_irq != 0) {
		rc = request_any_context_irq(pmic_reset_irq,
					resout_irq_handler, IRQF_TRIGGER_HIGH,
					"restart_from_pmic", NULL);
		if (rc < 0)
			pr_err("pmic restart irq fail rc = %d\n", rc);
	} else {
		pr_warn("no pmic restart interrupt specified\n");
	}

	return 0;
}

late_initcall(msm_pmic_restart_init);

static int __init msm_restart_init(void)
{
#ifdef CONFIG_MSM_DLOAD_MODE
	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
	dload_mode_addr = MSM_IMEM_BASE + DLOAD_MODE_ADDR;
	emergency_dload_mode_addr = MSM_IMEM_BASE +
		EMERGENCY_DLOAD_MODE_ADDR;
	set_dload_mode(download_mode);
#endif
	msm_tmr0_base = msm_timer_get_timer0_base();
	restart_reason = MSM_IMEM_BASE + RESTART_REASON_ADDR;
	#ifdef CONFIG_VENDOR_EDIT
/* OPPO 2013.07.09 hewei added begin for default restart reason*/
	__raw_writel(0x7766550a, restart_reason);
/* OPPO 2013.07.09 hewei added end for default restart reason*/
	#endif //CONFIG_VENDOR_EDIT
	pm_power_off = msm_power_off;

	if (scm_is_call_available(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER) > 0)
		scm_pmic_arbiter_disable_supported = true;

	return 0;
}
early_initcall(msm_restart_init);
