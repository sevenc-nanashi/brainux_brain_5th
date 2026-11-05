#!/usr/bin/env ruby
require 'fileutils'
abort 'usage: prepare-rootfs.rb EXTRACTED_BRAINUX_ROOTFS' unless ARGV.size == 1
root = File.realpath(ARGV[0])
raise 'Refusing host root' if root == '/'
raise 'Expected Brainux/Debian rootfs with sbin/init' unless File.exist?(File.join(root, 'sbin/init')) || File.symlink?(File.join(root, 'sbin/init'))
# Provision a copy of the rootfs before putting it on the SD card.
%w[etc etc/systemd etc/systemd/system etc/systemd/system/multi-user.target.wants].each do |relative|
  directory = File.join(root, relative)
  if File.exist?(directory) || File.symlink?(directory)
    raise "Directory escapes rootfs: #{directory}" unless File.realpath(directory).start_with?(root + '/')
    raise "Not a directory: #{directory}" unless File.directory?(directory)
  else
    Dir.mkdir(directory)
  end
end
unit = File.join(root, 'etc/systemd/system/brainux-usb-console.service')
wants = File.join(root, 'etc/systemd/system/multi-user.target.wants')
link = File.join(wants, 'brainux-usb-console.service')
[unit, link].each { |path| raise "Already exists: #{path}" if File.exist?(path) || File.symlink?(path) }
File.write(unit, <<~UNIT)
  [Unit]
  Description=Brainux PW-H1 USB serial login
  After=systemd-user-sessions.service
  ConditionPathExists=/dev/ttyGS0

  [Service]
  ExecStart=/sbin/agetty -L 115200 ttyGS0 vt100
  Restart=always
  Type=idle

  [Install]
  WantedBy=multi-user.target
UNIT
File.symlink('../brainux-usb-console.service', link)
puts "Enabled USB login in #{root}; existing Brainux account/password is required."
