#!/usr/bin/env ruby
require 'fileutils'
FileUtils.mkdir_p('target')
[
  ['https://github.com/brain-hackers/linux-brain.git', '5e99ce4dc22d77bb029133d7aa6483486c684f20', 'target/linux'],
  ['https://github.com/mirror/busybox.git', '1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4', 'target/busybox']
].each do |url, revision, destination|
  if File.exist?(destination)
    raise "#{destination} exists; inspect it before fetching"
  end
  system('git', 'init', destination, exception: true)
  system('git', '-C', destination, 'fetch', '--depth', '1', url, revision, exception: true)
  system('git', '-C', destination, 'checkout', '--detach', 'FETCH_HEAD', exception: true)
end
