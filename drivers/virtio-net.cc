
#include <sys/cdefs.h>

#include "drivers/virtio.hh"
#include "drivers/virtio-net.hh"
#include "drivers/pci-device.hh"
#include "interrupt.hh"

#include "mempool.hh"
#include "mmu.hh"
#include "sglist.hh"

#include <sstream>
#include <string>
#include <string.h>
#include <map>
#include <errno.h>
#include "debug.hh"

#include "sched.hh"
#include "drivers/clock.hh"
#include "drivers/clockevent.hh"

#include <osv/device.h>

using namespace memory;
using sched::thread;

// TODO list
// irq thread affinity and tx affinity
// Mergable buffers
// tx zero copy
// vlans?

namespace virtio {

    int virtio_net::_instance = 0;

    #define virtio_net_tag "virtio-net"
    #define virtio_net_d(fmt)   logger::instance()->log(virtio_net_tag, logger::logger_debug, (fmt))
    #define virtio_net_i(fmt)   logger::instance()->log(virtio_net_tag, logger::logger_info, (fmt))
    #define virtio_net_w(fmt)   logger::instance()->log(virtio_net_tag, logger::logger_warn, (fmt))
    #define virtio_net_e(fmt)   logger::instance()->log(virtio_net_tag, logger::logger_error, (fmt))


    virtio_net::virtio_net(virtio_device* dev)
        : virtio_driver(dev)
    {
        std::stringstream ss;
        ss << "virtio-net";

        _driver_name = ss.str();
        virtio_i(fmt("VIRTIO NET INSTANCE"));
        _id = _instance++;

        read_config();

        //register the 3 irq callback for the net
        msix_isr_list* isrs = new msix_isr_list;
        thread* rx = new thread([this] { this->receiver(); });
        thread* tx = new thread([this] { this->tx_gc_thread(); });
        rx->start();
        tx->start();
        isrs->insert(std::make_pair(0, rx));
        isrs->insert(std::make_pair(1, tx));
        _msi.easy_register(*isrs);

        fill_rx_ring();

        _dev->add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    }

    virtio_net::~virtio_net()
    {
        //TODO: In theory maintain the list of free instances and gc it
        // including the thread objects and their stack
    }

    bool virtio_net::read_config()
    {
        //read all of the net config  in one shot
        _dev->virtio_conf_read(_dev->virtio_pci_config_offset(), &_config, sizeof(_config));

        if (_dev->get_guest_feature_bit(VIRTIO_NET_F_MAC))
            virtio_i(fmt("The mac addr of the device is %x:%x:%x:%x:%x:%x") %
                    (u32)_config.mac[0] %
                    (u32)_config.mac[1] %
                    (u32)_config.mac[2] %
                    (u32)_config.mac[3] %
                    (u32)_config.mac[4] %
                    (u32)_config.mac[5]);

        return true;
    }
    struct ethhdr {
            unsigned char   h_dest[6];       /* destination eth addr */
            unsigned char   h_source[6];     /* source ether addr    */
            u16          h_proto;                /* packet type ID field */
    } __attribute__((packed));

    struct iphdr {
            u8    ihl:4,
                    version:4;
            u8    tos;
            u16  tot_len;
            u16  id;
            u16  frag_off;
            u8    ttl;
            u8    protocol;
            u16 check;
            u32  saddr;
            u32  daddr;
            /*The options start here. */
    } __attribute__((packed));


    struct icmphdr {
      u8          type;
      u8          code;
      u16       checksum;
      union {
            struct {
                    u16  id;
                    u16  sequence;
            } echo;
            u32  gateway;
            struct {
                    u16  __unused;
                    u16  mtu;
            } frag;
      } un;
    } __attribute__((packed));

    void virtio_net::receiver() {
        vring* queue = _dev->get_virt_queue(0);

        while (1) {
            thread::wait_until([this, queue] {
                return queue->used_ring_not_empty();
            });

            int i = 0;
            virtio_net_req * req;

            while((req = static_cast<virtio_net_req*>(queue->get_buf())) != nullptr) {

                auto ii = req->payload._nodes.begin();
                ii++;
                char*buf = reinterpret_cast<char*>(mmu::phys_to_virt(ii->_paddr));
                virtio_net_d(fmt("\t len=%d") % ii->_len);

                virtio_net_d(fmt("\t got hdr len:%d = %d vaddr=%p") % i++ % (int)req->hdr.hdr_len % (void*)buf);

                ethhdr* eh = reinterpret_cast<ethhdr*>(buf);
                virtio_net_d(fmt("The src %x:%x:%x:%x:%x:%x dst %x:%x:%x:%x:%x:%x type %d ") %
                        (u32)eh->h_source[0] %
                        (u32)eh->h_source[1] %
                        (u32)eh->h_source[2] %
                        (u32)eh->h_source[3] %
                        (u32)eh->h_source[4] %
                        (u32)eh->h_source[5] %
                        (u32)eh->h_dest[0] %
                        (u32)eh->h_dest[1] %
                        (u32)eh->h_dest[2] %
                        (u32)eh->h_dest[3] %
                        (u32)eh->h_dest[4] %
                        (u32)eh->h_dest[5] %
                        (u32)eh->h_proto);

                iphdr* ip = reinterpret_cast<iphdr*>(buf+sizeof(ethhdr));
                virtio_net_d(fmt("tot_len = %d protocol=%d, saddr=%d:%d:%d:%d daddr=%d:%d:%d:%d") %
                        (u32)ip->tot_len % (u32)ip->protocol % (ip->saddr & 0xff) % (ip->saddr >> 8 & 0xff) %
                        (ip->saddr >> 16 & 0xff) % (ip->saddr >> 24 & 0xff) % (ip->daddr & 0xff) % (ip->daddr >> 8 & 0xff) %
                        (ip->daddr >> 16 & 0xff) % (ip->daddr >> 24 & 0xff));

                icmphdr* icmp = reinterpret_cast<icmphdr*>(buf+sizeof(ethhdr)+sizeof(iphdr));
                virtio_net_d(fmt("icmp code=%d. type=%d") % (u32)icmp->code % (u32)icmp->type);

            }

            if (queue->avail_ring_has_room(queue->size()/2)) {
                virtio_net_d(fmt("ring is less than half full, refill"));
                fill_rx_ring();
            }

        }

    }

    static const int page_size = 4096;

    void virtio_net::fill_rx_ring()
    {
        vring* queue = _dev->get_virt_queue(0);
        virtio_net_d(fmt("%s") % __FUNCTION__);

        // it could have been a while (1) loop but it simplifies the allocation
        // tracking
        while (queue->avail_ring_has_room(2)) {
            virtio_net_req *req = new virtio_net_req;

            void* buf = malloc(page_size);
            memset(buf, 0, page_size);
            req->payload.add(mmu::virt_to_phys(buf), page_size);
            req->payload.add(mmu::virt_to_phys(static_cast<void*>(&req->hdr)), sizeof(struct virtio_net_hdr), true);

            if (!queue->add_buf(&req->payload,0,2,req)) {
                delete req;
                break;
            }
        }

        queue->kick();
    }

    bool virtio_net::tx(void *out, int len, bool flush)
    {
        vring* queue = _dev->get_virt_queue(1);
        virtio_net_d(fmt("%s") % __FUNCTION__);

        if (!queue->avail_ring_has_room(2)) {
            if (queue->used_ring_not_empty()) {
                virtio_net_d(fmt("%s: gc tx buffers to clear space"));
                tx_gc();
            } else {
                virtio_net_d(fmt("%s: no room") % __FUNCTION__);
                return false;
            }
        }

        virtio_net_req *req = new virtio_net_req;
        req->payload.add(mmu::virt_to_phys(out), len);
        req->payload.add(mmu::virt_to_phys(static_cast<void*>(&req->hdr)), sizeof(struct virtio_net_hdr), true);

        if (!queue->add_buf(&req->payload,0,2,req)) {
            delete req;
            return false;
        }

        if (flush)
            queue->kick();

        return true;
    }

    void virtio_net::tx_gc_thread() {
        vring* queue = _dev->get_virt_queue(1);

        while (1) {
            thread::wait_until([this, queue] {
                return queue->used_ring_not_empty();
            });

            virtio_net_d(fmt("\t %s: thread awaken") % __FUNCTION__);
        }
    }

    void virtio_net::tx_gc()
    {
        int i = 0;
        virtio_net_req * req;
        vring* queue = _dev->get_virt_queue(1);

        while((req = static_cast<virtio_net_req*>(queue->get_buf())) != nullptr) {
            virtio_net_d(fmt("%s: gc %d") % __FUNCTION__ % i++);

        }
    }

    u32 virtio_net::get_driver_features(void)
    {
        u32 base = virtio_driver::get_driver_features();
        return (base | ( 1 << VIRTIO_NET_F_MAC));
    }

    hw_driver* virtio_net::probe(hw_device* dev)
    {
        if (auto vdev = dynamic_cast<virtio_device*>(dev)) {
            if (vdev->get_id() == hw_device_id(VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID)) {
                return new virtio_net(vdev);
            }
        }
        return nullptr;
    }

    void virtio_net::tx_test()
    {
        while (1) {
            timespec ts = {};
            ts.tv_sec = 1;
            nanosleep(&ts, nullptr);

            sched::thread::current()->yield();
        }
    }

}
