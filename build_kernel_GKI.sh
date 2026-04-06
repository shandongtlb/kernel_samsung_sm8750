#!/bin/bash

#1. target config
BUILD_TARGET=pa3q_chn_openx
export MODEL=$(echo $BUILD_TARGET | cut -d'_' -f1)
export PROJECT_NAME=${MODEL}
export REGION=$(echo $BUILD_TARGET | cut -d'_' -f2)
export CARRIER=$(echo $BUILD_TARGET | cut -d'_' -f3)
export TARGET_BUILD_VARIANT=user
		
		
#2. sm8650 common config
CHIPSET_NAME=sun

export ANDROID_BUILD_TOP=$(pwd)
export TARGET_PRODUCT=perf
export TARGET_BOARD_PLATFORM=gki

export ANDROID_PRODUCT_OUT=${ANDROID_BUILD_TOP}/out/target/product/${MODEL}
export OUT_DIR=${ANDROID_BUILD_TOP}/out/msm-${CHIPSET_NAME}-${CHIPSET_NAME}-${TARGET_PRODUCT}

# for Lcd(techpack) driver build
export KBUILD_EXTRA_SYMBOLS="${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/mmrm-driver/Module.symvers ${ANDROID_BUILD_TOP}/out/target/product/${MODEL}/obj/DLKM_OBJ/vendor/qcom/opensource/wlan/platform/Module.symvers"

# for Audio(techpack) driver build
export MODNAME=audio_dlkm

export KBUILD_EXT_MODULES="\
	../vendor/qcom/opensource/mmrm-driver \
        ../vendor/qcom/opensource/mm-drivers/msm_ext_display \
        ../vendor/qcom/opensource/mm-drivers/sync_fence \
        ../vendor/qcom/opensource/mm-drivers/hw_fence \
        ../vendor/qcom/opensource/securemsm-kernel \
        ../vendor/qcom/opensource/wlan/platform \
        ../vendor/qcom/opensource/wlan/qcacld-3.0 \
        "

#3. build kernel
cd ./kernel_platform/
mkdir -p vendor/qcom/proprietary/devicetree
IS_KBUILD=true RECOMPILE_KERNEL=1 ./build/android/prepare_vendor.sh ${CHIPSET_NAME} ${TARGET_PRODUCT} gki | tee -a ../build.log
