#include <stddef.h>
#include <sys/ioctl.h>
#include <net/bpf.h>
#include <net/if.h>

static int (*const ioctl_signature)(int, unsigned long, ...) = ioctl;

_Static_assert(sizeof(struct ifreq) == 32, "Darwin arm64 ifreq size");
_Static_assert(_Alignof(struct ifreq) == 8, "Darwin arm64 ifreq alignment");
_Static_assert(offsetof(struct ifreq, ifr_name) == 0, "ifreq name offset");
_Static_assert(offsetof(struct ifreq, ifr_ifru) == 16, "ifreq union offset");

_Static_assert(sizeof(struct bpf_hdr) == 20, "Darwin arm64 bpf_hdr size");
_Static_assert(_Alignof(struct bpf_hdr) == 4, "Darwin arm64 bpf_hdr alignment");
_Static_assert(offsetof(struct bpf_hdr, bh_caplen) == 8, "bpf caplen offset");
_Static_assert(offsetof(struct bpf_hdr, bh_datalen) == 12, "bpf datalen offset");
_Static_assert(offsetof(struct bpf_hdr, bh_hdrlen) == 16, "bpf hdrlen offset");
_Static_assert(offsetof(struct bpf_hdr, bh_hdrlen) + sizeof(((struct bpf_hdr *)0)->bh_hdrlen) == 18,
    "Darwin native BPF header bytes exclude trailing C padding");

_Static_assert(BPF_WORDALIGN(21) == 24, "Darwin BPF alignment");
_Static_assert(BIOCGBLEN == 0x40044266UL, "BIOCGBLEN value");
_Static_assert(BIOCPROMISC == 0x20004269UL, "BIOCPROMISC value");
_Static_assert(BIOCSETIF == 0x8020426cUL, "BIOCSETIF value");
_Static_assert(BIOCIMMEDIATE == 0x80044270UL, "BIOCIMMEDIATE value");

int main(void) {
    return ioctl_signature == NULL;
}
