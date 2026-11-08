# Nemo rclone fork

This is a personal fork of [linuxmint/nemo](https://github.com/linuxmint/nemo)
that adds first-class rclone integration and a few remote-filesystem
performance fixes.  Upstream is kept as a separate git remote so the fork can
be rebased onto new releases.

## What v1 adds

### Remote filesystem policy

- Detects slow network/FUSE filesystems (`fuse.rclone`, `fuse.sshfs`, `cifs`,
  `nfs`, ...) via `/proc/self/mountinfo` (`libnemo-private/nemo-remote-policy.c`).
- Uses a lean attribute set for enumeration and per-file queries on those
  filesystems: no `access::*`, `owner::*`, `selinux::*`, `metadata::*`,
  `trash::*`.
- Does not enumerate every subdirectory just to display an item count on slow
  remotes (unless `show-directory-item-counts` is set to `always`).

### Rclone integration

- `src/rclone/`: rclone service, named-path favorites, transfer window.
- Remotes are read with `rclone listremotes` (fallback: parse `rclone.conf`).
- Favorites are stored in `~/.config/nemo/rclone-favorites.json`:

  ```json
  {
    "version": 1,
    "favorites": [
      {
        "id": "uuid",
        "label": "m64_backup_x_app",
        "remote": "m64",
        "path": "/data/backup/test/v3",
        "mountpoint": "/mnt/union/Dell-M6400"
      }
    ]
  }
  ```

- Sidebar section **Rclone** with a **Transfers** entry and one row per
  favorite.  Activating a favorite mounts it on demand (`rclone mount`) and
  opens the local mountpoint.  If the path does not belong to a favorite, the
  transfer window opens with that remote/path pre-filled instead.
- The `rclone://` scheme is handled by Nemo itself (not a GIO/GVFS backend),
  so it works from the sidebar, the location bar, `nemo rclone://...` and
  session restore without `G_IO_ERROR_NOT_SUPPORTED`:
  - `rclone://transfer` opens the transfer window.
  - `rclone://remote/path` mounts the matching favorite and opens it.
  - unknown `remote/path` opens the transfer window with the source filled in.
- On-demand mounts are created under `~/.local/share/nemo-rclone/mounts/` by
  default, or `/mnt/nemo-rclone/` when `/mnt` is writable by the user.  Set
  `NEMO_RCLONE_MOUNT_BASE` to override the location.  Each mount writes its
  log to `~/.cache/nemo-rclone/<mountpoint>.log`; failures show the log tail.
- Favorites tab has **Mount**, **Unmount** and **Open** buttons plus a live
  mounted/offline status column.
- Right-click menu **Rclone**:
  - Open Rclone Transfer…
  - Mark as Rclone Favorite…
  - Copy rclone Path
  - Use as Rclone Source
- Transfer window: source/destination pickers, directory browser backed by
  `rclone lsf`, `copy` / `copyto` / `move` / `sync`, dry-run, real progress
  parsed from `rclone --use-json-log --stats 1s`, cancel, log.

## Build and install

Dependencies are the normal Nemo dependencies plus `json-glib` (already a Nemo
dependency) and `gtk-layer-shell` if you want the Wayland desktop window.

```sh
cd /home/roychong/app/nemo
meson setup build --prefix=/usr/local \
  -Dgtk_layer_shell=true -Dxmp=true -Dexif=true \
  -Dvideo_preview=true -Dempty_view=false -Dtracker=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

The fork installs to `/usr/local`, so it shadows the distro package in
`/usr/bin` without overwriting any pacman-owned file.  `pacman -Syu` can still
upgrade the system `nemo` package; it will not replace `/usr/local/bin/nemo`.

`/usr/local/lib` must be in the dynamic loader path.  On Arch/CachyOS:

```sh
echo '/usr/local/lib' | sudo tee /etc/ld.so.conf.d/usr-local.conf
sudo ldconfig
```

The executables are linked with `RUNPATH=/usr/local/lib`, so the fork's own
`libnemo-extension.so.1` is preferred.

The compiled-in extension directory is `/usr/local/lib/nemo/extensions-3.0`.
To keep distro extensions (if installed later) visible:

```sh
sudo mkdir -p /usr/local/lib/nemo
sudo ln -sfn /usr/lib/nemo/extensions-3.0 /usr/local/lib/nemo/extensions-3.0
```

Restart Nemo after installation (`nemo -q`, then `nemo`).

## rclone mount recommendations

The current fork still uses the kernel/FUSE mount for browsing.  Tune the mount
for latency; the defaults below are a starting point:

```sh
rclone mount m64: /mnt/union/Dell-M6400 \
  --vfs-cache-mode full \
  --dir-cache-time 10m \
  --attr-timeout 1m \
  --poll-interval 0 \
  --vfs-read-ahead 128M \
  --vfs-read-chunk-streams 8 \
  --timeout 30s --contimeout 10s --low-level-retries 2
```

`--dir-cache-time 30s` (the previous setting) causes frequent full readdir
round trips.  `--poll-interval 0` is usually better for SFTP, which has no
cheap change notification.

## Known limitations / next steps

- On-demand mounts are plain detached `rclone mount` processes; there are no
  systemd units yet, so they are not restarted after a crash and are not
  unmounted automatically at logout.
- Copy/paste inside Nemo into a writable rclone mount still goes through FUSE,
  so Nemo's progress dialog can hit 100% before rclone finishes uploading from
  the VFS cache.  For now, start transfers from the Rclone window
  (right-click → **Use as Rclone Source**) to get real progress.
- Per-folder zoom/sort still uses GVFS metadata; a path-keyed view-state store
  is planned.
- The directory listing in the transfer window is synchronous; move it to a
  worker thread before using it on very large/slow directories.
- Planned: `vfs/queue` background-upload monitor, systemd `rclone-mount@.service`
  management with per-mount `--rc` sockets, viewport prefetch, per-directory
  I/O concurrency, and a `ricing.css` layer.

## License

GPLv2+, same as upstream Nemo.
