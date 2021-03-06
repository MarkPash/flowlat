#include <stdbool.h>
#include <stdint.h>

#include <linux/bpf.h>
#include <linux/bpf_common.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/pkt_cls.h>
#include <linux/tcp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
  __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} pipe SEC(".maps");

struct packet_t {
  struct in6_addr src_addr, dst_addr;
  __be16 src_port, dst_port;
  bool syn, ack;
  uint64_t ts;
};

SEC("classifier")
int probe(struct __sk_buff *skb) {
  if (bpf_skb_pull_data(skb, 0) < 0) {
    return TC_ACT_OK;
  }

  uint8_t *head = (uint8_t *)(long)skb->data;
  uint8_t *tail = (uint8_t *)(long)skb->data_end;

  if (head + sizeof(struct ethhdr) > tail) {
    return TC_ACT_OK;
  }

  struct ethhdr *eth = (void *)head;

  struct packet_t pkt = {0};

  uint32_t offset;

  switch (bpf_ntohs(eth->h_proto)) {
  case ETH_P_IP:
    offset = sizeof(struct ethhdr) + sizeof(struct iphdr);

    if (head + offset > tail) {
      return TC_ACT_OK;
    }

    struct iphdr *ip = (void *)head + sizeof(struct ethhdr);
    if (ip->protocol != IPPROTO_TCP) {
      return TC_ACT_OK;
    }

    // Create a IPv4-Mapped IPv6 Address
    pkt.src_addr.in6_u.u6_addr32[3] = ip->saddr;
    pkt.dst_addr.in6_u.u6_addr32[3] = ip->daddr;

    pkt.src_addr.in6_u.u6_addr16[5] = 0xffff;
    pkt.dst_addr.in6_u.u6_addr16[5] = 0xffff;

    break;
  case ETH_P_IPV6:
    offset = sizeof(struct ethhdr) + sizeof(struct ipv6hdr);

    if (head + offset > tail) {
      return TC_ACT_OK;
    }

    struct ipv6hdr *ip6 = (void *)head + sizeof(struct ethhdr);
    if (ip6->nexthdr != IPPROTO_TCP) {
      return TC_ACT_OK;
    }

    pkt.src_addr = ip6->saddr;
    pkt.dst_addr = ip6->daddr;

    break;
  default:
    return TC_ACT_OK;
  }

  if (head + offset + sizeof(struct tcphdr) > tail) {
    return TC_ACT_OK;
  }

  struct tcphdr *tcp = (void *)head + offset;

  if (tcp->syn) {
    pkt.src_port = tcp->source;
    pkt.dst_port = tcp->dest;
    pkt.syn = true;
    pkt.ack = tcp->ack;
    pkt.ts = bpf_ktime_get_ns();

    if (bpf_perf_event_output(skb, &pipe, BPF_F_CURRENT_CPU, &pkt,
                              sizeof(pkt)) < 0) {
      return TC_ACT_OK;
    }
  }
  return TC_ACT_OK;
}

char _license[] SEC("license") = "Dual MIT/GPL";
