#!/usr/bin/env python3
"""
System Metrics Feed — polls /proc, /sys for CPU, memory, disk, network, load
Outputs structured JSON matching feed.yml output_schema
"""
import json
import os
import sys
import time
import subprocess

def read_cpu():
    """Parse /proc/stat for CPU percentages"""
    with open('/proc/stat') as f:
        lines = f.readlines()
    
    # First line is aggregate cpu
    cpu_line = lines[0].split()
    if cpu_line[0] != 'cpu':
        return {}
    
    # user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
    vals = list(map(int, cpu_line[1:10]))
    total = sum(vals)
    idle = vals[3] + vals[4]  # idle + iowait
    
    # Need previous reading for delta - use a simple approach with sleep
    time.sleep(0.1)
    
    with open('/proc/stat') as f:
        lines2 = f.readlines()
    cpu_line2 = lines2[0].split()
    vals2 = list(map(int, cpu_line2[1:10]))
    total2 = sum(vals2)
    idle2 = vals2[3] + vals2[4]
    
    dt = total2 - total
    di = idle2 - idle
    if dt <= 0:
        return {}
    
    used_pct = 100.0 * (dt - di) / dt
    user_pct = 100.0 * (vals2[0] - vals[0]) / dt
    sys_pct = 100.0 * (vals2[2] - vals[2]) / dt
    idle_pct = 100.0 * di / dt
    iowait_pct = 100.0 * (vals2[4] - vals[4]) / dt
    steal_pct = 100.0 * (vals2[7] - vals[7]) / dt if len(vals2) > 7 and len(vals) > 7 else 0.0
    
    # Count cores
    cores = sum(1 for l in lines if l.startswith('cpu') and l[3].isdigit())
    
    return {
        'user_pct': round(user_pct, 1),
        'system_pct': round(sys_pct, 1),
        'idle_pct': round(idle_pct, 1),
        'iowait_pct': round(iowait_pct, 1),
        'steal_pct': round(steal_pct, 1),
        'total_cores': cores
    }

def read_memory():
    """Parse /proc/meminfo"""
    mem = {}
    with open('/proc/meminfo') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                key = parts[0].rstrip(':')
                val = int(parts[1])  # kB
                mem[key] = val
    
    total_kb = mem.get('MemTotal', 0)
    free_kb = mem.get('MemFree', 0)
    avail_kb = mem.get('MemAvailable', free_kb)
    cached_kb = mem.get('Cached', 0)
    buffers_kb = mem.get('Buffers', 0)
    swap_total_kb = mem.get('SwapTotal', 0)
    swap_free_kb = mem.get('SwapFree', 0)
    
    used_kb = total_kb - avail_kb
    swap_used_kb = swap_total_kb - swap_free_kb
    
    return {
        'total_mb': total_kb // 1024,
        'used_mb': used_kb // 1024,
        'free_mb': free_kb // 1024,
        'available_mb': avail_kb // 1024,
        'used_pct': round(100.0 * used_kb / total_kb, 1) if total_kb else 0.0,
        'cached_mb': (cached_kb + buffers_kb) // 1024,
        'swap_used_mb': swap_used_kb // 1024
    }

def read_disk():
    """Parse df -B1 for disk usage"""
    try:
        out = subprocess.check_output(['df', '-B1', '-x', 'tmpfs', '-x', 'devtmpfs', '-x', 'squashfs'], text=True)
        lines = out.strip().split('\n')[1:]
        disks = []
        for line in lines:
            parts = line.split()
            if len(parts) >= 6:
                device, blocks, used, avail, pct, mount = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), parts[4], parts[5]
                total_gb = blocks / 1e9
                used_gb = used / 1e9
                free_gb = avail / 1e9
                used_pct = float(pct.rstrip('%'))
                
                # Get fstype
                fstype = 'unknown'
                try:
                    fstype = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', mount], text=True).strip()
                except:
                    pass
                
                # Get inode usage
                inodes_used_pct = 0.0
                try:
                    di = subprocess.check_output(['df', '-i', mount], text=True).strip().split('\n')[1]
                    inodes_used_pct = float(di.split()[4].rstrip('%'))
                except:
                    pass
                
                disks.append({
                    'device': device,
                    'mountpoint': mount,
                    'fstype': fstype,
                    'total_gb': round(total_gb, 1),
                    'used_gb': round(used_gb, 1),
                    'free_gb': round(free_gb, 1),
                    'used_pct': used_pct,
                    'inodes_used_pct': round(inodes_used_pct, 1)
                })
        return disks
    except Exception:
        return []

def read_network():
    """Parse /proc/net/dev for network stats"""
    try:
        with open('/proc/net/dev') as f:
            lines = f.readlines()[2:]
        
        prev = {}
        for line in lines:
            parts = line.split()
            iface = parts[0].rstrip(':')
            if iface == 'lo':
                continue
            rx_b, tx_b = int(parts[1]), int(parts[9])
            rx_p, tx_p = int(parts[2]), int(parts[10])
            rx_e, tx_e = int(parts[3]), int(parts[11])
            prev[iface] = (rx_b, tx_b, rx_p, tx_p, rx_e, tx_e)
        
        time.sleep(0.1)
        
        with open('/proc/net/dev') as f:
            lines = f.readlines()[2:]
        
        nets = []
        for line in lines:
            parts = line.split()
            iface = parts[0].rstrip(':')
            if iface == 'lo' or iface not in prev:
                continue
            rx_b, tx_b = int(parts[1]), int(parts[9])
            rx_p, tx_p = int(parts[2]), int(parts[10])
            rx_e, tx_e = int(parts[3]), int(parts[11])
            
            prx_b, ptx_b, prx_p, ptx_p, prx_e, ptx_e = prev[iface]
            dt = 0.1
            
            nets.append({
                'interface': iface,
                'rx_bytes_sec': round((rx_b - prx_b) / dt),
                'tx_bytes_sec': round((tx_b - ptx_b) / dt),
                'rx_packets_sec': round((rx_p - prx_p) / dt),
                'tx_packets_sec': round((tx_p - ptx_p) / dt),
                'rx_errors_sec': round((rx_e - prx_e) / dt),
                'tx_errors_sec': round((tx_e - ptx_e) / dt)
            })
        return nets
    except Exception:
        return []

def read_load():
    """Parse /proc/loadavg"""
    try:
        with open('/proc/loadavg') as f:
            parts = f.read().split()
        load1, load5, load15 = float(parts[0]), float(parts[1]), float(parts[2])
        procs = parts[3].split('/')
        procs_running = int(procs[0])
        procs_total = int(procs[1])
        return {
            'load1': load1,
            'load5': load5,
            'load15': load15,
            'procs_running': procs_running,
            'procs_total': procs_total
        }
    except Exception:
        return {'load1': 0, 'load5': 0, 'load15': 0, 'procs_running': 0, 'procs_total': 0}

def main():
    timestamp = int(time.time())
    
    output = {
        'timestamp': timestamp,
        'cpu': read_cpu(),
        'memory': read_memory(),
        'disk': read_disk(),
        'network': read_network(),
        'load': read_load()
    }
    
    json.dump(output, sys.stdout)
    return 0

if __name__ == '__main__':
    sys.exit(main())