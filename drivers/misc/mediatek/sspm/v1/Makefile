ifneq ($(strip $(CONFIG_MTK_TINYSYS_SSPM_SUPPORT)$(CONFIG_MTK_TINYSYS_SSPM_PLT_SUPPORT)),)
obj-y += sspm_emi_mpu.o
obj-y += sspm_ipi_timeout_cb.o
obj-y += sspm_helper.o sspm_irq.o sspm_sysfs.o sspm_reservedmem.o sspm_plt.o
obj-y += sspm_mbox.o sspm_ipi_mbox.o sspm_timesync.o

# The switch lives in sspm_define.h as a C macro that make cannot read, so it
# is mirrored here. Default matches sspm_define.h (logger off, as in the stable
# R vendor tree); turn both on together if it is ever needed again.
SSPM_LOGGER_SUPPORT ?= 0
ifeq ($(SSPM_LOGGER_SUPPORT),1)
obj-y += sspm_logger_impl.o
endif

ccflags-y += -I$(srctree)/drivers/misc/mediatek/emi/submodule
ccflags-y += -I$(srctree)/drivers/misc/mediatek/emi/$(CONFIG_MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/sspm/v1
ccflags-y += -I$(srctree)/drivers/misc/mediatek/sspm/$(CONFIG_MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/$(CONFIG_MTK_PLATFORM)
ccflags-$(CONFIG_MTK_ENG_BUILD) += -DDEBUG
endif
