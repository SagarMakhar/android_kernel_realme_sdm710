#
# Makefile for kernel Silead Fingerprint drivers.
#

ifdef CONFIG_ARCH_MSM
	 ccflags-y += -DBSP_SIL_PLAT_QCOM
endif

ifdef CONFIG_ARCH_QCOM
	 ccflags-y += -DBSP_SIL_PLAT_QCOM
endif

ifdef CONFIG_ARCH_SPRD
	 ccflags-y += -DBSP_SIL_PLAT_SPRD
endif

ifndef CONFIG_SILEAD_FP_PLATFORM
obj-y	+= silead_fp.o
obj-$(CONFIG_SILEAD_FP_SPI)	+= silead_fp.o
endif

obj-$(CONFIG_SILEAD_FP_PLATFORM)	+= silead_fp_platform.o

