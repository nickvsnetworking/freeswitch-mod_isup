# FreeSWITCH core + mod_isup. Reuses the prebuilt libfreeswitch (GLIBC<=2.29)
# on a Debian bullseye base which provides OpenSSL 1.1 + sofia-sip.
# Build context = repo root.
FROM debian:bullseye-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg gcc make pkg-config libc6-dev && \
    rm -rf /var/lib/apt/lists/*

# Osmocom packages (M3UA stack + MGCP client) for building/running mod_isup
RUN wget -qO /etc/apt/trusted.gpg.d/osmocom.asc \
        "https://downloads.osmocom.org/packages/osmocom:/latest/Debian_11/Release.key" && \
    echo "deb https://downloads.osmocom.org/packages/osmocom:/latest/Debian_11/ ./" \
        > /etc/apt/sources.list.d/osmocom.list && \
    apt-get update && apt-get install -y --no-install-recommends \
        libosmocore-dev libosmo-netif-dev libosmo-sigtran-dev \
        libtalloc-dev \
        libssl1.1 libsofia-sip-ua0 libspeex1 libspeexdsp1 libedit2 \
        libsqlite3-0 libcurl4 libpcre2-8-0 libuuid1 libodbc1 && \
    rm -rf /var/lib/apt/lists/*

# Prebuilt FreeSWITCH core library
COPY .libs/libfreeswitch.so.1.0.0 /usr/local/lib/
RUN cd /usr/local/lib && \
    ln -sf libfreeswitch.so.1.0.0 libfreeswitch.so.1 && \
    ln -sf libfreeswitch.so.1 libfreeswitch.so

# spandsp3 stub (bullseye ships spandsp2; FS core never calls it on our path)
RUN printf 'int plc_fillin(void*a,short*b,int c){(void)a;(void)b;(void)c;return 0;}\n' > /tmp/sp.c && \
    gcc -shared -fPIC -Wl,-soname,libspandsp.so.3 -o /usr/local/lib/libspandsp.so.3 /tmp/sp.c && \
    ldconfig

# FreeSWITCH headers + module sources (incl. dialplan/dptools to build)
COPY src/include /fs/src/include
COPY libs/libteletone/src /fs/libs/libteletone/src
COPY src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c /fs/mod_dialplan_xml.c
COPY src/mod/applications/mod_dptools/mod_dptools.c /fs/mod_dptools.c
COPY mod_isup /mod_isup

# Build mod_isup.so + the loader harness
RUN cd /mod_isup && \
    CF="$(pkg-config --cflags libosmo-sigtran) -I. -I/fs/src/include -I/fs/libs/libteletone/src" && \
    LF="$(pkg-config --libs libosmo-sigtran) -losmovty -losmocore -ltalloc" && \
    SRCS="isup_codec.c isup_param.c isup_sm.c isup_cgm.c isup_segment.c isup_map.c isup_trace.c isup_m3ua.c bearer_mgcp.c mod_isup.c" && \
    gcc -shared -fPIC -O2 -std=gnu11 -Wno-unused-parameter $CF -o mod_isup.so $SRCS $LF \
        -L/usr/local/lib -lfreeswitch -Wl,--allow-shlib-undefined && \
    gcc -O2 -std=gnu11 -I. -I/fs/src/include -I/fs/libs/libteletone/src \
        -o fs_harness test/live/fs_harness.c -L/usr/local/lib -lfreeswitch \
        -Wl,--allow-shlib-undefined && \
    mkdir -p /tmp/fsrun
# stock FreeSWITCH dialplan + applications modules (for the dialplan call path)
RUN cd /fs && \
    gcc -shared -fPIC -O2 -I/fs/src/include -I/fs/libs/libteletone/src \
        -o /mod_isup/mod_dialplan_xml.so mod_dialplan_xml.c \
        -L/usr/local/lib -lfreeswitch -Wl,--allow-shlib-undefined && \
    gcc -shared -fPIC -O2 -I/fs/src/include -I/fs/libs/libteletone/src \
        -o /mod_isup/mod_dptools.so mod_dptools.c \
        -L/usr/local/lib -lfreeswitch -Wl,--allow-shlib-undefined
# minimal core config so full switch_core_init() completes
COPY mod_isup/docker/freeswitch.xml /tmp/fsrun/freeswitch.xml

ENV LD_LIBRARY_PATH=/usr/local/lib
ENV ISUP_MOD_DIR=/mod_isup
CMD ["/bin/bash"]
