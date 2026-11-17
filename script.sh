set -o pipefail
ssh -T -o HostKeyAlias=carplay-handshake-probe root@10.0.0.1 'cd /persist/logs && tar -cf - boot-*' | gzip > "iphone-probe-$(date +%Y%m%d-%H%M%S).tar.gz"