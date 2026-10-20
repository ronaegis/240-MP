# Sourced by 240mp-bridge and install-replayos.sh: where ReplayOS keeps its
# games. Its config is always on the SD card; system_storage names the storage
# picked in System > Storage, mounted at /media/<name> (sd, nvme, usb, nfs).
REPLAY_CFG=/media/sd/config/replay.cfg

# Prints a replay.cfg value (lines look like: key = "value").
replay_cfg() {
   sed -n "s/^$1 *= *\"\(.*\)\"\$/\1/p" "$REPLAY_CFG" 2>/dev/null
}

# Prints the mount point of the selected storage; the SD card when that
# storage isn't mounted (an empty /media/nvme would put files on the root fs).
storage_dir() {
   _s=/media/$(replay_cfg system_storage)
   if [ "$_s" != /media/ ] && mountpoint -q "$_s"; then echo "$_s"; else echo /media/sd; fi
}
