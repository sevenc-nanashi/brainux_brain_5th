#!/usr/bin/env ruby
require 'digest'
require 'fileutils'
abort 'usage: bundle.rb Image board.dtb BOOT.BIN' unless ARGV.size == 3
image, dtb = ARGV.first(2).map { |path| File.binread(path) }
raise 'Image exceeds staging capacity' unless (4096..9*1024*1024-64-65536).cover?(image.bytesize)
raise 'DTB must be 40 bytes..64 KiB' unless (40..65536).cover?(dtb.bytesize)
raise 'DTB magic/size mismatch' unless dtb.unpack('N2') == [0xd00dfeed, dtb.bytesize]
payload = image + "\0" * ((-image.bytesize) % 8) + dtb
header = 'BUXBOOT1' + [image.bytesize, dtb.bytesize].pack('V2') + Digest::SHA256.digest(payload) + "\0" * 16
raise 'header layout changed' unless header.bytesize == 64
raise 'payload exceeds app RAM' unless header.bytesize + payload.bytesize <= 9*1024*1024
FileUtils.mkdir_p(File.dirname(ARGV[2]))
File.binwrite(ARGV[2], header + payload)
puts "#{ARGV[2]}: #{header.bytesize + payload.bytesize} bytes"
