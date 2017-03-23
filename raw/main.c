#include <stdio.h> //standard stuff (printf..)
#include <stdlib.h> //malloc
#include <string.h> //memset
#include <netinet/tcp.h> //declaration for tcp header
#include <netinet/ip.h> //declaration for ip header
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <pcap.h>
#include <errno.h> //strerr
#include <netdb.h>
#include <netinet/if_ether.h>
#include "../trafgen/csum.h"

#define SIZE_ETHERNET 14
#define IP_HL(ip)		(((ip)->ip_hl) & 0x0f)

int process_packet(unsigned char*, struct in_addr *dst_ip);
void print_ip_header(unsigned char*, int);
void print_tcp_header(unsigned char*, int);
void hostname_toip(char *, struct in_addr *dst_ip);
int get_local_ip(char *);
void receive_ack(struct in_addr *dst_ip);
int start_sniffing(struct in_addr *dst_ip);
void send_syn_ack(char *source_ip, struct in_addr *dst_ip, char *source_port, int sock_raw);
unsigned short tcp_csum(int src, int dst, unsigned short *addr, int len);
void packet_receive(u_char *udata, const struct pcap_pkthdr *pkthdr, const u_char *packet);

int tcp=0, others=0, total=0, i, j;
struct sockaddr_in source, dest;

//For checksum
struct psd_tcp {
    unsigned int src_address;
    unsigned int dst_address;
    unsigned char reserved;
    unsigned char protocol;
    unsigned short tcp_length;
    struct tcphdr tcp;
};

int main(int argc, char *argv[])
{
    int sock_raw;
    struct in_addr dst_ip;

    sock_raw = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);

    if(sock_raw < 0)
    {
        perror("Socket error\n");
        return 1;
    }
    else
    {
        printf("Socket created.\n");
    }

    char *dst = argv[1];

    if(argc < 2)
    {
        fprintf(stderr, "Usage: %s <Hostname>\n", argv[0]);
        return 1;
    }

    //If hostname (argv[1]) is specified in numbers-and-dots notation, convert to network byte order
    if(inet_addr(dst) != INADDR_NONE)
    {
        //inet_addr converts dst from numbers-dots notaion to network byte order
        printf("Directly filling dst_ip.\n");
        dst_ip.s_addr = inet_addr(dst);
    }
    else
    {
        //Calls function hostname_toip which translates hostname to IP
        printf("Calling addrinfo function, with a host: '%s'\n", dst);
        hostname_toip(dst, &dst_ip);
        //inet_ntoa need whole in_addr struct
        printf("Value returned: %s\n", inet_ntoa(dst_ip));
    }

    int source_port = 55555;
    char source_ip[20] = "192.168.0.29";
    //get_local_ip(source_ip);

    printf("Local ip is: %s\n", source_ip);

    //IP header
    struct ip iph;
    struct tcphdr tcph;
    struct sockaddr_in dest;

    char *packet_to_send;
    packet_to_send = (char *)malloc(60);

    //Zero out the packet memory area
    //memset(packet_to_send, 0, 4096);
    //Fill in the IP header
    iph.ip_hl = 5;
    iph.ip_v = 4;
    iph.ip_tos = 0;
    iph.ip_len = sizeof(struct ip) + sizeof(struct tcphdr);
    iph.ip_id = htons(12345);
    iph.ip_off = 0;
    iph.ip_ttl = 64;
    iph.ip_p = IPPROTO_TCP;
    iph.ip_sum = 0; //Will be calculated afterwards
    iph.ip_src.s_addr = inet_addr(source_ip);
    iph.ip_dst.s_addr = dst_ip.s_addr;

    //Function that calculates checksum is implemented in trafgen
    iph.ip_sum = csum((unsigned short *) packet_to_send, iph.ip_len >> 1);

    memcpy(packet_to_send, &iph, sizeof(iph));

    //Fill in the TCP header
    tcph.th_sport = htons(source_port);
    tcph.th_dport = htons(80);
    tcph.th_seq = htonl(0);
    tcph.th_ack = htonl(0);
    tcph.th_x2 = 0;
    tcph.th_off = sizeof(struct tcphdr) / 4;
    tcph.th_flags = TH_SYN;
    tcph.th_win = htons(29200);
    tcph.th_sum = 0;
    tcph.th_urp = 0;
    tcph.th_sum = tcp_csum(iph.ip_src.s_addr, iph.ip_dst.s_addr, (unsigned short *)&tcph, sizeof(tcph));

    memcpy((packet_to_send + sizeof(iph)), &tcph, sizeof(tcph));

    int one = 1;

    //IP_HDRINCL tells the kernel that headers are included
    if(setsockopt(sock_raw, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
    {
        fprintf(stderr, "Error setting up setsockopt!\n");
        return 1;
    }

    printf("Starting sniffing.\n");
    int th1ret = 0;
    pthread_t thread1;

    //Creating thread with receive_ack() function call, will start receiving packets and process them
    if(pthread_create(&thread1, NULL, receive_ack, &dst_ip) < 0)
    {
        fprintf(stderr, "Failed to create a new thread.\n");
        return 1;
    }
    sleep(1);

    printf("Starting to send syn packets.\n");

    //dest - sockaddr_in
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = iph.ip_dst.s_addr;

    //struct sockaddr_in my_addr;
    //my_addr.sin_family = AF_INET;
    //inet_aton(source_ip, my_addr.sin_addr.s_addr);
    //my_addr.sin_port = "55555";

    //if(bind(sock_raw, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
    //{
    //    perror("Error setting up bind.\n");
    //    return 1;
    //}

    //Send the packet out
    if(sendto(sock_raw, packet_to_send, sizeof(struct iphdr) + sizeof(struct tcphdr), 0, (struct sockaddr *) &dest, sizeof(struct sockaddr)) < 0)
    {
        fprintf(stderr, "Error sending syn packet! Error message: %s\n", strerror(errno));
        exit(1);
    }

    printf("Syn has been sent out.\n");
    pthread_join(thread1, NULL);
    send_syn_ack(source_ip, &dst_ip, source_port, sock_raw);
    printf("Value returned from thread 1: %d\nProgram will now close.\n", th1ret);
    return 0;
}

void hostname_toip(char *dst, struct in_addr *dst_ip)
{
    struct addrinfo hints, *result;
    int addrinfores = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    printf("Getting addrinfo of: %s\n", dst);

    //Translating hostname to IP
    addrinfores = getaddrinfo(dst, "80", &hints, &result);

    if(addrinfores != 0)
    {
        fprintf(stderr, "Getaddrinfo error: %s\n", strerror(errno));
        exit(0);
    }

    struct sockaddr_in *addr;
    //I can cast sockaddr to sockaddr_in which I need for writing to stdout
    addr = (struct sockaddr_in *) result->ai_addr;
    printf("You address: %s\n", inet_ntoa((struct in_addr) addr->sin_addr));

    //ai_addr is a struct in_addr what is the same type as dst_ip
    *dst_ip = addr->sin_addr;

    //I have dst_ip as a pointer here, that's why I need to pass *dst_ip
    printf("Translated as: %s\n", inet_ntoa(*dst_ip));
}

void send_syn_ack(char *source_ip, struct in_addr *dst_ip, char *source_port, int sock_raw)
{
    struct ip iph;
    struct tcphdr tcph;
    struct sockaddr_in dest;

    char *pkt_syn_ack;
    pkt_syn_ack = (char *) malloc(60);

    iph.ip_hl = 5;
    iph.ip_v = 4;
    iph.ip_tos = 0;
    iph.ip_len = sizeof(struct ip) + sizeof(struct tcphdr);
    iph.ip_id = htons(12346);
    iph.ip_off = 0;
    iph.ip_ttl = 64;
    iph.ip_p = IPPROTO_TCP;
    iph.ip_sum = 0;
    iph.ip_src.s_addr = inet_addr(source_ip);
    iph.ip_dst.s_addr = dst_ip->s_addr;
    iph.ip_sum = csum((unsigned short *) pkt_syn_ack, iph.ip_len >> 1);

    memcpy(pkt_syn_ack, &iph, sizeof(iph));

    tcph.th_sport = htons(source_port);
    tcph.th_dport = htons(80);
    tcph.th_seq = htonl(1);
    tcph.th_ack = htonl(1);
    tcph.th_x2 = 0;
    tcph.th_off = sizeof(struct tcphdr) / 4;
    tcph.th_flags = TH_ACK;
    tcph.th_win = htons(29200);
    tcph.th_sum = 0;
    tcph.th_urp = 0;
    tcph.th_sum = tcp_csum(iph.ip_src.s_addr, iph.ip_dst.s_addr, (unsigned short *)&tcph, sizeof(tcph));

    memcpy(pkt_syn_ack + sizeof(iph), &tcph, sizeof(tcph));

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = iph.ip_dst.s_addr;

    if(sendto(sock_raw, pkt_syn_ack, sizeof(struct iphdr) + sizeof(struct tcphdr), 0, (struct sockaddr *) &dest, sizeof(struct sockaddr)) < 0)
    {
        perror("Failed to send syn_ack packet!\n");
        return 1;
    }

    printf("Syn_Ack packet has been sent out!\n");

}

void receive_ack(struct in_addr *dst_ip)
{
    start_sniffing(dst_ip);
}

int start_sniffing(struct in_addr *dst_ip)
{

    pcap_t *descr;
    char *filter = "host www.ricanyubrna.cz";
    char *dev = "wlan0";
    char error_buffer[PCAP_ERRBUF_SIZE];

    bpf_u_int32 net;
    bpf_u_int32 mask;

    struct bpf_program filterf;

    if((descr = pcap_open_live(dev, BUFSIZ, 1, -1, error_buffer)) == NULL)
    {
        perror("Could not open a pcap device.\n");
        return 1;
    }

    //looking up net/mask for given dev
    pcap_lookupnet(dev, &net, &mask, error_buffer);

    //compiling filter expression
    pcap_compile(descr, &filterf, filter, 0, net);


    if(pcap_setfilter(descr, &filterf) == -1)
    {
        perror("Error setting up pcap filter.\n");
        return 1;
    }

    //freeing a bpf program
    pcap_freecode(&filterf);

    //getting link-layer header type = 1 means Ethernet according to IEEE 802.3
    int hdr_type = pcap_datalink(descr);
    int length;

    if(hdr_type == 1)
        //sizeof ethernet header is 14
        length = 14;
        if(pcap_loop(descr, -1, packet_receive, (u_char *) length) < 0)
        {
            perror("Cannot get raw packet.\n");
            return 1;
        }

    //int socket_raw;
    //int data_size;

    //struct sockaddr saddr;
    //unsigned int saddr_size;

    ////Access to the buffer using dynamic mem alloc
    //unsigned char *buffer = (unsigned char *) malloc(65536);

    //printf("Starting up the sniffer!\n");
    //fflush(stdout);

    ////Creating raw socket used for sniffing
    //socket_raw = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    //if(socket_raw < 0)
    //{
    //    fprintf(stderr, "Failed to create a socket. Error message: %s", strerror(errno));
    //    fflush(stdout);
    //    return 1;
    //}

    //printf("Receiving packets.\n");
    //saddr_size = sizeof(saddr);
    //struct sockaddr_in my_addr;
    //my_addr.sin_family = AF_INET;
    //inet_aton(source_ip, my_addr.sin_addr.s_addr);
    //my_addr.sin_port = "55555";

    //if(bind(sock_raw, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
    //{
    //    perror("Error setting up bind.\n");
    //    return 1;
    //}

    //while(1)
    //{
    //    int ret = 1;
    //    //Receive a packet, packets are stored into a buffer
    //    data_size = recvfrom(socket_raw, buffer, 65535, 0, &saddr, &saddr_size);

    //    if(data_size < 0)
    //    {
    //        printf("Recvfrom error, failed to get packets! Error message: %s\n", strerror(errno));
    //        fflush(stdout);
    //        return 1;
    //    }

    //    //Now process the packet
    //    ret = process_packet(buffer, dst_ip);
    //    if(ret == 0)
    //        break;
    //        //return 0;
    //}

    //close(socket_raw);
    //printf("Finished sniffing.\n");
    //fflush(stdout);
    return 0;
}

void packet_receive(u_char *udata, const struct pcap_pkthdr *pkthdr, const u_char *packet)
{
    struct ip *ip;
    struct tcphdr *tcp;
    u_char *ptr;

    //int l1_len = (int) udata;
    int seq_n;

    ip = (struct ip *)(packet + sizeof(struct ether_header));
    //printf("Size of ether_header is: %d\n", sizeof(struct ether_header));
    tcp = (struct tcphdr *)(packet + SIZE_ETHERNET + (IP_HL(ip)*4));

    printf("---------------------------\n");
    printf("GOT PACKET!\n");
    printf("---------------------------\n");

    //printf("%d\n", l1_len);
    printf("Packet came along with src IP: %s\n", inet_ntoa(ip->ip_src));
    printf("Packet came along with dst IP: %s\n", inet_ntoa(ip->ip_dst));

    printf("Packet came with ack: %d\n", ntohl(tcp->th_ack));
    printf("Packet came with seq: %u\n", ntohl(tcp->th_seq));
    //& is a binary AND
    if(tcp->th_flags & TH_SYN)
        printf("Packet has a flags set as SYN!\n");
    if(tcp->th_flags & TH_ACK)
        printf("Packet has a flags set as ACK!\n");
    if(tcp->th_flags & TH_RST)
        printf("Packet has a flags set as RST!\n");
    if(tcp->th_flags & TH_FIN)
        printf("Packet has a flags set as FIN!\n");
    if(tcp->th_flags & TH_PUSH)
        printf("Packet has a flags set as PUSH!\n");
    //seq_n = ntohl(tcp->th_seq);

    }

int process_packet(unsigned char *buffer, struct in_addr *dst_ip)
{
    //Get the IP header
    struct iphdr *iph = (struct iphdr *)buffer;
    struct sockaddr_in source, destination;
    unsigned short iphdrlength;

    //If upper level protocol is TCP then...
    if(iph->protocol == 6)
    {
        iphdrlength = iph->ihl*4;

        struct tcphdr *tcph = (struct tcphdr *)(buffer + iphdrlength);

        //Fill memory used by "source" with zeros
        memset(&source, 0, sizeof(source));
        source.sin_addr.s_addr = iph->saddr;

        memset(&destination, 0, sizeof(destination));
        destination.sin_addr.s_addr = iph->daddr;

        if(tcph->syn == 1 && tcph->ack == 1 && source.sin_addr.s_addr == dst_ip->s_addr)
        {
            printf("You just received an ack message!\n");
            fflush(stdout);
            //printf("Dst port: %s\n", );
            //send_syn_ack(inet_ntoa(source.sin_addr), &dst_ip, source_port, sock_raw);
            //send_syn_ack(source_ip, &dst_ip, source_port, sock_raw);
            return 0;
        }
        return 1;

    }

}

unsigned short in_cksum(unsigned short *addr, int len)
{
    int nleft = len;
    int sum = 0;
    unsigned short *w = addr;
    unsigned short answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *) (&answer) = *(unsigned char *) w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return (answer);
}

unsigned short tcp_csum(int src, int dst, unsigned short *addr, int len)
{
    struct psd_tcp buf;
    u_short ans;

    memset(&buf, 0, sizeof(buf));
    buf.src_address = src;
    buf.dst_address = dst;
    buf.reserved = 0;
    buf.protocol = IPPROTO_TCP;
    buf.tcp_length = htons(len);
    memcpy(&(buf.tcp), addr, len);
    ans = in_cksum((unsigned short *)&buf, 12 + len);
    return ans;
}
