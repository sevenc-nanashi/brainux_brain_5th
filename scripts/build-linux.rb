#!/usr/bin/env ruby
require 'fileutils'
root = File.expand_path('..', __dir__)
linux = File.expand_path(ENV.fetch('LINUX', File.join(root, 'target/linux')))
busybox = File.expand_path(ENV.fetch('BUSYBOX_SRC', File.join(root, 'target/busybox')))
cross = ENV.fetch('CROSS_COMPILE', 'arm-linux-gnueabi-')
jobs = ENV.fetch('JOBS', '4')
def run(*args, **options)
  puts args.join(' ')
  system(*args, **options, exception: true)
end
raise 'Linux source missing; run make fetch' unless File.file?(File.join(linux, 'Makefile'))
raise 'BusyBox source missing; run make fetch' unless File.file?(File.join(busybox, 'Makefile'))
# Only the pinned source revisions are supported by the board patch.
revision = IO.popen(['git', '-C', linux, 'rev-parse', 'HEAD'], &:read).strip
raise "Unexpected Linux revision: #{revision}" unless revision == '5e99ce4dc22d77bb029133d7aa6483486c684f20'
bb_revision = IO.popen(['git', '-C', busybox, 'rev-parse', 'HEAD'], &:read).strip
raise "Unexpected BusyBox revision: #{bb_revision}" unless bb_revision == '1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4'
patch = File.join(root, 'kernel/pwh1.patch')
if system('git', '-C', linux, 'apply', '--reverse', '--check', patch, out: File::NULL, err: File::NULL)
  puts 'Board patch already applied'
else
  run('git', '-C', linux, 'apply', '--check', patch)
  run('git', '-C', linux, 'apply', patch)
end
FileUtils.cp(File.join(root, 'kernel/pwh1-brainux.dts'), File.join(linux, 'arch/arm/boot/dts/imx7ulp-pwh1-brainux.dts'))
FileUtils.cp(File.join(root, 'kernel/pwh1-refresh.c'), File.join(linux, 'drivers/video/fbdev/pwh1-refresh.c'))
FileUtils.cp(File.join(root, 'kernel/pwh1-keys.c'), File.join(linux, 'drivers/input/keyboard/pwh1-keys.c'))
bb_out = File.join(root, 'target/busybox-build')
FileUtils.mkdir_p(bb_out)
base = ['make', '-C', busybox, "O=#{bb_out}", "CROSS_COMPILE=#{cross}"]
run(*base, 'defconfig', out: File::NULL)
config = File.read(File.join(bb_out, '.config'))
config.sub!('# CONFIG_STATIC is not set', 'CONFIG_STATIC=y')
# These applets need kernel headers removed from current libc sysroots.
%w[TC FEATURE_TC_INGRESS FEATURE_TC_REDIRECT FEATURE_TC_QDISC FEATURE_TC_CBQ].each do |name|
  config.gsub!(/^CONFIG_#{name}=y$/, "# CONFIG_#{name} is not set")
end
File.write(File.join(bb_out, '.config'), config)
run(*base, 'oldconfig', in: File::NULL, out: File::NULL)
run(*base, "-j#{jobs}")
fs = File.join(root, 'target/initramfs')
FileUtils.mkdir_p(fs)
run(*base, "CONFIG_PREFIX=#{fs}", 'install', out: File::NULL)
%w[dev proc sys run newroot].each { |dir| FileUtils.mkdir_p(File.join(fs, dir)) }
FileUtils.cp(File.join(root, 'initramfs/init'), File.join(fs, 'init'))
FileUtils.cp(File.join(root, 'initramfs/boot-root'), File.join(fs, 'sbin/boot-root'))
FileUtils.cp(File.join(root, 'initramfs/usb-report'), File.join(fs, 'sbin/usb-report'))
FileUtils.chmod(0755, [File.join(fs, 'init'), File.join(fs, 'sbin/boot-root')])
# gen_init_cpio makes the console node without requiring root or mknod.
list = File.join(root, 'target/initramfs.list')
entries = Dir.glob('**/*', base: fs).sort.map do |path|
  full = File.join(fs, path)
  raise "Whitespace in initramfs path: #{path}" if full.match?(/\s/)
  if File.symlink?(full)
    "slink /#{path} #{File.readlink(full)} 777 0 0"
  elsif File.directory?(full)
    "dir /#{path} 755 0 0"
  else
    "file /#{path} #{full} #{(File.stat(full).mode & 0777).to_s(8)} 0 0"
  end
end
entries << 'nod /dev/console 600 0 0 c 5 1'
File.write(list, entries.join("\n") + "\n")
output = File.join(root, 'target/linux-build')
FileUtils.mkdir_p(output)
base = ['make', '-C', linux, "O=#{output}", 'ARCH=arm', "CROSS_COMPILE=#{cross}"]
run(*base, 'allnoconfig', out: File::NULL)
fragment = File.join(root, 'target/brainux.config')
File.write(fragment, File.read(File.join(root, 'kernel/brainux.config')) + "\nCONFIG_INITRAMFS_SOURCE=\"#{list}\"\n")
run({'ARCH'=>'arm', 'CROSS_COMPILE'=>cross}, File.join(linux, 'scripts/kconfig/merge_config.sh'), '-m', '-O', output,
    File.join(output, '.config'), fragment, chdir: linux, out: File::NULL)
run(*base, 'olddefconfig', out: File::NULL)
resolved = File.read(File.join(output, '.config'))
%w[MMU CPU_V7 SOC_IMX7ULP GENERIC_IDLE_POLL_SETUP ARM_PSCI ARM_PATCH_PHYS_VIRT DEVTMPFS BLK_DEV_INITRD FB_SIMPLE MFD_SYSCON DEBUG_FS USB_MXS_PHY USB_CHIPIDEA_IMX USB_CONFIGFS_ACM USB_CHIPIDEA_UDC MMC_SDHCI_ESDHC_IMX EXT4_FS].each do |name|
  raise "Required kernel option missing: CONFIG_#{name}=y" unless resolved.lines.any? { |line| line.chomp == "CONFIG_#{name}=y" }
end
run(*base, "-j#{jobs}", 'Image', 'imx7ulp-pwh1-brainux.dtb')
run('ruby', File.join(root, 'scripts/bundle.rb'), File.join(output, 'arch/arm/boot/Image'),
    File.join(output, 'arch/arm/boot/dts/imx7ulp-pwh1-brainux.dtb'), File.join(root, 'target/APP/Brainux/BOOT.BIN'))
