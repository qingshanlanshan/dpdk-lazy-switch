#include "main.h"

int app_fwd_learning(struct flow_key *key, struct app_fwd_table_item *value)
{
    if (app.fwd_hash == NULL)
    {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR hash table is not initialized.\n",
            __func__);
        return -1;
    }
    int index = rte_hash_lookup(app.fwd_hash, key);
    if (index == -EINVAL)
    {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR the parameters are invalid when lookup hash table\n",
            __func__);
    }
    else if (index == -ENOENT)
    {
        int new_ind = rte_hash_add_key(app.fwd_hash, key);
        if (new_ind == -ENOSPC)
        {
            RTE_LOG(INFO, HASH,
                    "%s: ENOSPC, reseting\n",
                    __func__);
            rte_hash_reset(app.fwd_hash);
            new_ind = rte_hash_add_key(app.fwd_hash, key);
        }
        app.fwd_table[new_ind].last_sent_port = value->last_sent_port;
        app.fwd_table[new_ind].last_sent_time = value->last_sent_time;
        app.fwd_table[new_ind].flowlet_gap = value->flowlet_gap;
    }
    else if (index < 0 || index >= FORWARD_ENTRY)
    {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR invalid table entry found in hash table: %d\n",
            __func__, index);
        return -1;
    }
    else
    {
        app.fwd_table[index].last_sent_port = value->last_sent_port;
        app.fwd_table[index].last_sent_time = value->last_sent_time;
        app.fwd_table[index].flowlet_gap = value->flowlet_gap;
    }
    return 0;
}

int app_fwd_lookup(const struct flow_key *key, struct app_fwd_table_item *value)
{
    int index = rte_hash_lookup(app.fwd_hash, key);
    if (index >= 0 && index < FORWARD_ENTRY)
    {
        uint64_t now_time = rte_get_tsc_cycles();
        uint64_t interval = now_time - app.fwd_table[index].last_sent_time;
        if (interval <= app.fwd_item_valid_time)
        {
            value->last_sent_port = app.fwd_table[index].last_sent_port;
            value->last_sent_time = app.fwd_table[index].last_sent_time;
            value->flowlet_gap = app.fwd_table[index].flowlet_gap;
            return 0;
        }
        else
        {
            rte_hash_del_key(app.fwd_hash, key);
            RTE_LOG(
                INFO, HASH,
                "%s: ERROR key port: %d\n",
                __func__, key->port);
            return -1;
        }
        /*struct timeval now_time, intv_time;
        gettimeofday(&now_time, NULL);
        timersub(&now_time, &app.fwd_table[index].timestamp, &intv_time);
        long intv_time_us = intv_time.tv_sec * 1000 * 1000 + intv_time.tv_usec;
        if (intv_time_us / 1000 < VALID_TIME) {
            return app.fwd_table[index].port_id;
        } else {
            rte_hash_del_key(app.l2_hash, addr);
            return -1;
        }*/
    }
    return -1;
}

void app_main_loop_forwarding(void)
{
    struct app_mbuf_array *worker_mbuf;
    uint32_t i;
    int dst_port;
    struct ipv4_5tuple_host *ipv4_5tuple;
    int default_port;
    struct flow_key key;
    struct app_fwd_table_item value;
    struct detection_status
    {
        uint64_t timestamp;
        bool on;
    } status;

    default_port = app.default_port;
    srand((unsigned)time(NULL));
    RTE_LOG(INFO, SWITCH, "Core %u is doing forwarding\n",
            rte_lcore_id());

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
    app.fwd_item_valid_time = app.cpu_freq[rte_lcore_id()] / 1000 * VALID_TIME;
    uint64_t rtt = app.cpu_freq[rte_lcore_id()] / 1000000 * app.rtt;
    uint64_t init_flowlet_gap = 9 * rtt;
    uint64_t decrease_rate = rtt * 8 / app.k;
    uint64_t ts0, ts1, ts2, ts3;
    app.cyc = 0;
    app.tot_cyc = 0;

    if (app.log_qlen)
    {
        fprintf(
            app.qlen_file,
            "# %-10s %-8s %-8s %-8s\n",
            "<Time (in s)>",
            "<Port id>",
            "<Qlen in Bytes>",
            "<Buffer occupancy in Bytes>");
        fflush(app.qlen_file);
    }
    worker_mbuf = rte_malloc_socket(NULL, sizeof(struct app_mbuf_array),
                                    RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (worker_mbuf == NULL)
        rte_panic("Worker thread: cannot allocate buffer space\n");
    if (app.fw_policy == 0)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: Letflow\n",
            __func__);
    }
    else if (app.fw_policy == 1)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: CONGA\n",
            __func__);
    }
    else if (app.fw_policy == 2)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: DRILL\n",
            __func__);
    }
    else if (app.fw_policy == 3)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: Halflife\n",
            __func__);
    }
    else if (app.fw_policy == 4)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: per-flow ECMP\n",
            __func__);
    }
    else if (app.fw_policy == 5)
    {
        RTE_LOG(
            INFO, SWITCH,
            "%s: Using fw_policy: random\n",
            __func__);
    }

    status.on = 1;
    status.timestamp = rte_get_tsc_cycles();
    for (i = 0; !force_quit; i = (i + 1) % app.n_ports)
    {
        ts0 = rte_get_tsc_cycles();
        int ret;

        /*ret = rte_ring_sc_dequeue_bulk(
            app.rings_rx[i],
            (void **) worker_mbuf->array,
            app.burst_size_worker_read);*/
        ret = rte_ring_sc_dequeue(
            app.rings_rx[i],
            (void **)worker_mbuf->array);

        if (ret == -ENOENT)
            continue;

        if (i != app.port)
        {
            dst_port = app.port;
        }
        else
        {
            // dst_port = default_port;
            ipv4_5tuple = rte_pktmbuf_mtod_offset(worker_mbuf->array[0], struct ipv4_5tuple_host *, sizeof(struct ether_hdr) + offsetof(struct ipv4_hdr, time_to_live));

            key.ip = ipv4_5tuple->ip_src;
            key.port = ipv4_5tuple->port_src;

            uint64_t now_time = rte_get_tsc_cycles();
            if (status.on && now_time - status.timestamp > app.rtt * 8)
            {
                status.on = 0;
                status.timestamp = now_time;
            }
            else if (!status.on && now_time - status.timestamp > app.rtt * 100)
            {
                status.on = 1;
                status.timestamp = now_time;
            }

            ret = app_fwd_lookup(&key, &value);
            now_time = rte_get_tsc_cycles();
            if (ret == 0 && !status.on)
            {
                dst_port = value.last_sent_port;
            }
            else if (ret == 0 && status.on)
            {
                ts1 = rte_get_tsc_cycles();
                if (value.lock)
                {
                    value.lock = 0;
                    dst_port = value.last_sent_port;
                }
                else
                {

                    if (app.fw_policy == Letflow && (now_time - value.last_sent_time) > 5 * app.rtt) // letflow
                    {
                        dst_port = rand() % (app.n_ports - 2);
                        if (dst_port >= app.port)
                            dst_port++;
                        if (dst_port >= value.last_sent_port)
                            dst_port++;
                        if (dst_port == value.last_sent_port)
                            dst_port++;
                        app.flowlet_counter++;
                    }
                    else if (app.fw_policy == Conga && (now_time - value.last_sent_time) > 5 * app.rtt) // conga
                    {
                        uint32_t min_qlen = UINT32_MAX;
                        uint32_t qlen;
                        for (int j = 0; j < app.n_ports; ++j)
                        {
                            if (j == value.last_sent_port || j == app.port)
                                continue;
                            qlen = get_qlen_bytes(j);
                            if (qlen < min_qlen)
                            {
                                min_qlen = qlen;
                                dst_port = j;
                            }
                        }
                        app.flowlet_counter++;
                    }
                    else if (app.fw_policy == Drill) // drill
                    {
                        int randret = rand() % (app.n_ports - 1);
                        int ban_port = (app.port + randret + 1) % app.n_ports;
                        uint32_t min_qlen = UINT32_MAX;
                        uint32_t qlen;
                        for (int j = 0; j < app.n_ports; ++j)
                        {
                            if (j == app.port || j == ban_port)
                                continue;
                            qlen = get_qlen_bytes(j);
                            if (qlen < min_qlen)
                            {
                                min_qlen = qlen;
                                dst_port = j;
                            }
                        }
                    }
                    else if (app.fw_policy == Halflife) // halflife
                    {
                        if (value.flowlet_gap > rtt)
                            value.flowlet_gap -= decrease_rate;
                        if ((now_time - value.last_sent_time) > value.flowlet_gap)
                        {
                            value.flowlet_gap = init_flowlet_gap;
                            int randret = rand() % (app.n_ports - 1);
                            int ban_port = (app.port + randret + 1) % app.n_ports;
                            uint32_t min_qlen = UINT32_MAX;
                            uint32_t qlen;
                            for (int j = 0; j < app.n_ports; ++j)
                            {
                                if (j == app.port || j == ban_port)
                                    continue;
                                qlen = get_qlen_bytes(j);
                                if (qlen < min_qlen)
                                {
                                    min_qlen = qlen;
                                    dst_port = j;
                                }
                            }
                        }
                        app.flowlet_counter++;
                    }
                    else if (app.fw_policy == ECMP) // per-flow ECMP
                    {
                        dst_port = value.last_sent_port;
                    }
                    else if (app.fw_policy == Random) // random
                    {
                        dst_port = rand() % (app.n_ports - 2);
                        if (dst_port >= app.port)
                            dst_port++;
                        if (dst_port >= value.last_sent_port)
                            dst_port++;
                        if (dst_port == value.last_sent_port)
                            dst_port++;
                    }

                    value.last_sent_time = now_time;
                    value.last_sent_port = dst_port;
                    app_fwd_learning(&key, &value);
                    ts2 = rte_get_tsc_cycles();
                    app.cyc += (ts2 - ts1);
                }
            }
            else if (ret < 0)
            {
                if (app.fw_policy == Halflife)
                {
                    value.flowlet_gap = init_flowlet_gap;
                }
                value.last_sent_time = now_time;
                value.lock = 1;
                if (app.fw_policy != ECMP)
                {
                    dst_port = rand() % (app.n_ports - 1);
                    if (dst_port >= app.port)
                        dst_port++;
                    value.last_sent_port = dst_port;
                }
                app_fwd_learning(&key, &value);
                if (app.fw_policy == ECMP)
                {
                    dst_port = rte_hash_lookup(app.fwd_hash, &key) % 4 + 1;
                    value.last_sent_port = dst_port;
                    app_fwd_learning(&key, &value);
                }
            }
            if (!status.on && !value.lock)
            {
                value.lock = 1;
                app_fwd_learning(&key, &value);
            }
            ts3 = rte_get_tsc_cycles();
            app.tot_cyc += (ts3 - ts0);
        }

        RTE_LOG(
            DEBUG, SWITCH,
            "%s: Port %d: forward packet to %d\n",
            __func__, i, app.ports[dst_port]);
        packet_enqueue(dst_port, worker_mbuf->array[0]);
        app.n_fw++;
    }
}
