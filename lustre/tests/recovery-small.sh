#!/bin/sh

set -e

# 20b: bug  2986
ALWAYS_EXCEPT=" 20b"


LUSTRE=${LUSTRE:-`dirname $0`/..}

. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/lmv.sh}

build_test_filter

assert_env MDSCOUNT

# Allow us to override the setup if we already have a mounted system by
# setting SETUP=" " and CLEANUP=" "
SETUP=${SETUP:-"setup"}
CLEANUP=${CLEANUP:-"cleanup"}

gen_config() {
    rm -f $XMLCONFIG

    if [ "$MDSCOUNT" -gt 1 ]; then
        add_lmv lmv1_svc
        for mds in `mds_list`; do
            MDSDEV=$TMP/${mds}-`hostname`
            add_mds $mds --dev $MDSDEV --size $MDSSIZE  --lmv lmv1_svc
        done
        add_lov_to_lmv lov1 lmv1_svc --stripe_sz $STRIPE_BYTES \
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	MDS=lmv1
    else
        add_mds mds1 --dev $MDSDEV --size $MDSSIZE
        add_lov lov1 mds1 --stripe_sz $STRIPE_BYTES \
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	MDS=mds1_svc

    fi

    add_ost ost --lov lov1 --dev $OSTDEV --size $OSTSIZE
    add_ost ost2 --lov lov1 --dev ${OSTDEV}-2 --size $OSTSIZE
    add_client client ${MDS} --lov lov1 --path $MOUNT
}

setup() {
    gen_config
    start_krb5_kdc || exit 1
    start ost --reformat $OSTLCONFARGS 
    start ost2 --reformat $OSTLCONFARGS 
    start_lsvcgssd || exit 2
    start_lgssd || exit 3
    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE
    for mds in `mds_list`; do
	start $mds --reformat $MDSLCONFARGS
    done
    grep " $MOUNT " /proc/mounts || zconf_mount `hostname`  $MOUNT
}

cleanup() {
    zconf_umount `hostname` $MOUNT
    for mds in `mds_list`; do
	stop $mds ${FORCE} $MDSLCONFARGS
    done
    stop_lgssd
    stop_lsvcgssd
    stop ost2 ${FORCE} --dump cleanup.log
    stop ost ${FORCE} --dump cleanup.log
}

if [ ! -z "$EVAL" ]; then
    eval "$EVAL"
    exit $?
fi

if [ "$ONLY" == "setup" ]; then
    setup
    exit
fi

if [ "$ONLY" == "cleanup" ]; then
    sysctl -w portals.debug=0 || true
    cleanup
    exit
fi

REFORMAT=--reformat $SETUP
unset REFORMAT

[ "$ONLY" == "setup" ] && exit

test_1() {
    drop_request "mcreate $MOUNT/1"  || return 1
    drop_reint_reply "mcreate $MOUNT/2"    || return 2
}
run_test 1 "mcreate: drop req, drop rep"

test_2() {
    drop_request "tchmod 111 $MOUNT/2"  || return 1
    drop_reint_reply "tchmod 666 $MOUNT/2"    || return 2
}
run_test 2 "chmod: drop req, drop rep"

test_3() {
    drop_request "statone $MOUNT/2" || return 1
    drop_reply "statone $MOUNT/2"   || return 2
}
run_test 3 "stat: drop req, drop rep"

test_4() {
    do_facet client "cp /etc/resolv.conf $MOUNT/resolv.conf" || return 1
    drop_request "cat $MOUNT/resolv.conf > /dev/null"   || return 2
    drop_reply "cat $MOUNT/resolv.conf > /dev/null"     || return 3
}
run_test 4 "open: drop req, drop rep"

test_5() {
    drop_request "mv $MOUNT/resolv.conf $MOUNT/renamed" || return 1
    drop_reint_reply "mv $MOUNT/renamed $MOUNT/renamed-again" || return 2
    do_facet client "checkstat -v $MOUNT/renamed-again"  || return 3
}
run_test 5 "rename: drop req, drop rep"

test_6() {
    drop_request "mlink $MOUNT/renamed-again $MOUNT/link1" || return 1
    drop_reint_reply "mlink $MOUNT/renamed-again $MOUNT/link2"   || return 2
}
run_test 6 "link: drop req, drop rep"

test_7() {
    drop_request "munlink $MOUNT/link1"   || return 1
    drop_reint_reply "munlink $MOUNT/link2"     || return 2
}
run_test 7 "unlink: drop req, drop rep"

#bug 1423
test_8() {
    drop_reint_reply "touch $MOUNT/renamed"    || return 1
}
run_test 8 "touch: drop rep (bug 1423)"

#bug 1420
test_9() {
    pause_bulk "cp /etc/profile $MOUNT"       || return 1
    do_facet client "cp /etc/termcap $MOUNT"  || return 2
    do_facet client "sync"
    do_facet client "rm $MOUNT/termcap $MOUNT/profile" || return 3
}
run_test 9 "pause bulk on OST (bug 1420)"

#bug 1521
test_10() {
    do_facet client mcreate $MOUNT/f10        || return 1
    drop_bl_callback "chmod 0777 $MOUNT/f10"  || return 2
    # wait for the mds to evict the client
    #echo "sleep $(($TIMEOUT*2))"
    #sleep $(($TIMEOUT*2))
    do_facet client touch  $MOUNT/f10 || echo "touch failed, evicted"
    do_facet client checkstat -v -p 0777 $MOUNT/f10  || return 3
    do_facet client "munlink $MOUNT/f10"
}
run_test 10 "finish request on server after client eviction (bug 1521)"

#bug 2460
# wake up a thead waiting for completion after eviction
test_11(){
    do_facet client multiop $MOUNT/$tfile Ow  || return 1
    do_facet client multiop $MOUNT/$tfile or  || return 2

    cancel_lru_locks OSC

    do_facet client multiop $MOUNT/$tfile or  || return 3
    drop_bl_callback multiop $MOUNT/$tfile Ow  || 
        echo "client evicted, as expected"

    do_facet client munlink $MOUNT/$tfile  || return 4
}
run_test 11 "wake up a thead waiting for completion after eviction (b=2460)"

#b=2494
test_12(){
    $LCTL mark multiop $MOUNT/$tfile OS_c 
    do_facet mds "sysctl -w lustre.fail_loc=0x115"
    clear_failloc mds $((TIMEOUT * 2)) &
    multiop $MOUNT/$tfile OS_c  &
    PID=$!
#define OBD_FAIL_MDS_CLOSE_NET           0x115
    sleep 2
    kill -USR1 $PID
    cancel_lru_locks MDC  # force the close
    echo "waiting for multiop $PID"
    wait $PID || return 2
    do_facet client munlink $MOUNT/$tfile  || return 3
}
run_test 12 "recover from timed out resend in ptlrpcd (b=2494)"

# Bug 113, check that readdir lost recv timeout works.
test_13() {
    mkdir /mnt/lustre/readdir || return 1
    touch /mnt/lustre/readdir/newentry  || return 
# OBD_FAIL_MDS_READPAGE_NET|OBD_FAIL_ONCE
    do_facet mds "sysctl -w lustre.fail_loc=0x80000104"
    ls /mnt/lustre/readdir || return 3
    do_facet mds "sysctl -w lustre.fail_loc=0"
    rm -rf /mnt/lustre/readdir  || return 4
}
run_test 13 "mdc_readpage restart test (bug 1138)"

# Bug 113, check that readdir lost send timeout works.
test_14() {
    mkdir /mnt/lustre/readdir
    touch /mnt/lustre/readdir/newentry
# OBD_FAIL_MDS_SENDPAGE|OBD_FAIL_ONCE
    do_facet mds "sysctl -w lustre.fail_loc=0x80000106"
    ls /mnt/lustre/readdir || return 1
    do_facet mds "sysctl -w lustre.fail_loc=0"
}
run_test 14 "mdc_readpage resend test (bug 1138)"

test_15() {
    do_facet mds "sysctl -w lustre.fail_loc=0x80000128"
    touch $DIR/$tfile && return 1
    return 0
}
run_test 15 "failed open (-ENOMEM)"

stop_read_ahead() {
   for f in /proc/fs/lustre/llite/*/read_ahead; do 
      echo 0 > $f
   done
}

start_read_ahead() {
   for f in /proc/fs/lustre/llite/*/read_ahead; do 
      echo 1 > $f
   done
}

# recovery timeout. This actually should be taken from 
# obd_timeout
RECOV_TIMEOUT=30

test_16() {
    do_facet client cp /etc/termcap $MOUNT
    sync
    stop_read_ahead

#define OBD_FAIL_PTLRPC_BULK_PUT_NET 0x504 | OBD_FAIL_ONCE
    sysctl -w lustre.fail_loc=0x80000504
    cancel_lru_locks OSC
    # will get evicted here
    do_facet client "cmp /etc/termcap $MOUNT/termcap"  && return 1
    sysctl -w lustre.fail_loc=0
    # give recovery a chance to finish (shouldn't take long)
    sleep $RECOV_TIMEOUT
    do_facet client "cmp /etc/termcap $MOUNT/termcap"  || return 2
    start_read_ahead
}
run_test 16 "timeout bulk put, evict client (2732)"

test_17() {
    # OBD_FAIL_PTLRPC_BULK_GET_NET 0x0503 | OBD_FAIL_ONCE
    # client will get evicted here
    sysctl -w lustre.fail_loc=0x80000503
    do_facet client cp /etc/termcap $DIR/$tfile

    sleep $RECOV_TIMEOUT
    sysctl -w lustre.fail_loc=0
    do_facet client "df $DIR"
    # expect cmp to fail
    do_facet client "cmp /etc/termcap $DIR/$tfile"  && return 1
    do_facet client "rm $DIR/$tfile" || return 2
    return 0
}
run_test 17 "timeout bulk get, evict client (2732)"

test_18a() {
    do_facet client mkdir -p $MOUNT/$tdir
    f=$MOUNT/$tdir/$tfile

    cancel_lru_locks OSC
    pgcache_empty || return 1

    # 1 stripe on ost2
    lfs setstripe $f $((128 * 1024)) 1 1

    do_facet client cp /etc/termcap $f
    sync
    local osc2_dev=`$LCTL device_list | \
	awk '(/ost2.*client_facet/){print $4}' `
    $LCTL --device %$osc2_dev deactivate
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    $LCTL --device %$osc2_dev activate
    rm -f $f
    return $rc
}
run_test 18a "manual ost invalidate clears page cache immediately"

test_18b() {
# OBD_FAIL_PTLRPC_BULK_PUT_NET|OBD_FAIL_ONCE
    do_facet client mkdir -p $MOUNT/$tdir
    f=$MOUNT/$tdir/$tfile
    f2=$MOUNT/$tdir/${tfile}-2

    cancel_lru_locks OSC
    pgcache_empty || return 1

    # shouldn't have to set stripe size of count==1
    lfs setstripe $f $((128 * 1024)) 0 1
    lfs setstripe $f2 $((128 * 1024)) 0 1

    do_facet client cp /etc/termcap $f
    sync
    # just use this write to trigger the client's eviction from the ost
    sysctl -w lustre.fail_loc=0x80000503
    do_facet client dd if=/dev/zero of=$f2 bs=4k count=1
    sync
    sysctl -w lustre.fail_loc=0
    # allow recovery to complete
    sleep $((TIMEOUT + 2))
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    rm -f $f $f2
    return $rc
}
run_test 18b "eviction and reconnect clears page cache (2766)"

test_19a() {
    f=$MOUNT/$tfile
    do_facet client mcreate $f        || return 1
    drop_ldlm_cancel "chmod 0777 $f"  || echo evicted

    do_facet client checkstat -v -p 0777 $f  || echo evicted
    # let the client reconnect
    sleep 5
    do_facet client "munlink $f"
}
run_test 19a "test expired_lock_main on mds (2867)"

test_19b() {
    f=$MOUNT/$tfile
    do_facet client multiop $f Ow  || return 1
    do_facet client multiop $f or  || return 2

    cancel_lru_locks OSC

    do_facet client multiop $f or  || return 3
    drop_ldlm_cancel multiop $f Ow  || echo "client evicted, as expected"

    do_facet client munlink $f  || return 4
}
run_test 19b "test expired_lock_main on ost (2867)"

test_20a() {	# bug 2983 - ldlm_handle_enqueue cleanup
	mkdir -p $DIR/$tdir
	multiop $DIR/$tdir/${tfile} O_wc &
	MULTI_PID=$!
	sleep 1
	cancel_lru_locks OSC
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost sysctl -w lustre.fail_loc=0x80000308
	kill -USR1 $MULTI_PID
	wait $MULTI_PID
	rc=$?
	[ $rc -eq 0 ] && error "multiop didn't fail enqueue: rc $rc" || true
}
run_test 20a "ldlm_handle_enqueue error (should return error)" 

test_20b() {	# bug 2986 - ldlm_handle_enqueue error during open
	mkdir -p $DIR/$tdir
	touch $DIR/$tdir/${tfile}
	cancel_lru_locks OSC
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost sysctl -w lustre.fail_loc=0x80000308
	dd if=/etc/hosts of=$DIR/$tdir/$tfile && \
		error "didn't fail open enqueue" || true
}
run_test 20b "ldlm_handle_enqueue error (should return error)"

test_21a() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       close_pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000129"
       multiop $DIR/$tdir-2/f Oc &
       open_pid=$!
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000115"
       kill -USR1 $close_pid
       cancel_lru_locks MDC  # force the close
       wait $close_pid || return 1
       wait $open_pid || return 2
       do_facet mds "sysctl -w lustre.fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 3
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 4

       rm -rf $DIR/$tdir-*
}
run_test 21a "drop close request while close and open are both in flight"

test_21b() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       close_pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000107"
       mcreate $DIR/$tdir-2/f &
       open_pid=$!
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       kill -USR1 $close_pid
       cancel_lru_locks MDC  # force the close
       wait $close_pid || return 1
       wait $open_pid || return 3

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 4
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 5
       rm -rf $DIR/$tdir-*
}
run_test 21b "drop open request while close and open are both in flight"

test_21c() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       close_pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000107"
       mcreate $DIR/$tdir-2/f &
       open_pid=$!
       sleep 3
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000115"
       kill -USR1 $close_pid
       cancel_lru_locks MDC  # force the close
       wait $close_pid || return 1
       wait $open_pid || return 2

       do_facet mds "sysctl -w lustre.fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21c "drop both request while close and open are both in flight"

test_21d() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000129"
       multiop $DIR/$tdir-2/f Oc &
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000122"
       kill -USR1 $pid
       cancel_lru_locks MDC  # force the close
       wait $pid || return 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3

       rm -rf $DIR/$tdir-*
}
run_test 21d "drop close reply while close and open are both in flight"

test_21e() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       kill -USR1 $pid
       cancel_lru_locks MDC  # force the close
       wait $pid || return 1

       sleep $TIMEOUT
       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21e "drop open reply while close and open are both in flight"

test_21f() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000122"
       kill -USR1 $pid
       cancel_lru_locks MDC  # force the close
       wait $pid || return 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21f "drop both reply while close and open are both in flight"

test_21g() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000115"
       kill -USR1 $pid
       cancel_lru_locks MDC  # force the close
       wait $pid || return 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21g "drop open reply and close request while close and open are both in flight"

test_21h() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop $DIR/$tdir-1/f O_c &
       pid=$!

       do_facet mds "sysctl -w lustre.fail_loc=0x80000107"
       touch $DIR/$tdir-2/f &
       touch_pid=$!
       sleep 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       do_facet mds "sysctl -w lustre.fail_loc=0x80000122"
       cancel_lru_locks MDC  # force the close
       kill -USR1 $pid
       wait $pid || return 1
       do_facet mds "sysctl -w lustre.fail_loc=0"

       wait $touch_pid || return 2

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 3
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 4
       rm -rf $DIR/$tdir-*
}
run_test 21h "drop open request and close reply while close and open are both in flight"

# bug 3462 - multiple MDC requests
test_22() {
    f1=$DIR/${tfile}-1
    f2=$DIR/${tfile}-2
    
    do_facet mds "sysctl -w lustre.fail_loc=0x80000115"
    multiop $f2 Oc &
    close_pid=$!

    sleep 1
    multiop $f1 msu || return 1

     cancel_lru_locks MDC  # force the close
    do_facet mds "sysctl -w lustre.fail_loc=0"

    wait $close_pid || return 2
    rm -rf $f2 || return 4
}
run_test 22 "drop close request and do mknod"

test_23() { #b=4561
    multiop $DIR/$tfile O_c &
    pid=$!
    # give a chance for open
    sleep 5

    # try the close
    drop_request "kill -USR1 $pid"

    fail mds
    wait $pid || return 1
    return 0
}
#run_test 23 "client hang when close a file after mds crash"


$CLEANUP
