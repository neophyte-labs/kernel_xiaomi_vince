#!/system/bin/sh

# Dark Ages Kernel Default Profile
# Author: Blacksuan19 @ xda developers
# inspired by smurf kernel profiles manager
# Licensed Under GPL 3.0+

#governor
echo pixel_smurfutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# governor settings
echo 500 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/up_rate_limit_us
echo 2000 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/down_rate_limit_us
echo 1 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/pl
echo 3 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/bit_shift1
echo 4 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/bit_shift1_2
echo 10 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/bit_shift2
echo 25 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/target_load1
echo 75 > /sys/devices/system/cpu/cpufreq/pixel_smurfutil/target_load2

# set min freq
echo 652800 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq

# cpu boost
echo 15 > /dev/stune/top-app/schedtune.boost
echo 15 > /dev/stune/top-app/schedtune.sched_boost
echo 30 > /sys/module/cpu_boost/parameters/dynamic_stune_boost
echo 30 > /sys/module/cpu_boost/parameters/input_boost_ms
echo 1 > /sys/module/cpu_boost/parameters/sched_boost_on_input
echo 0:1401600000 1:1401600 2:1401600 3:1401600 4:1401600 5:1401600 6:1401600 7:1401600 > /sys/module/cpu_boost/parameters/input_boost_freq

# read ahead
echo 512 > /sys/block/mmcblk0/queue/read_ahead_kb 
echo 512 > /sys/block/mmcblk1/queue/read_ahead_kb 

# dynamic fsync
echo 1 > /sys/kernel/dyn_fsync/Dyn_fsync_active

# usb fast charge
echo 1 > /sys/kernel/fast_charge/force_fast_charge
# tcp stuff
sysctl -w net.ipv4.tcp_congestion_control=westwood

# enable fingerprint boost
echo 1 > /sys/kernel/fp_boost/enabled
