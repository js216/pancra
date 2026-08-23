// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* WHAT A NETWORK FAILURE WAS, in the words the app can act on -- and
 * nothing else. */
final class NetPolicy {

    /* WHY A SYNC REQUEST DID NOT HAPPEN.
     *
     * PancraNet.syncHttp catches a Throwable, and one answer -- -1, "it did
     * not work" -- makes every screen say SYNC FAILED for a
     * mistyped server name, an expired certificate, a phone with no network
     * and a server that was merely slow, three of which the user can act on
     * and only one of which the app should keep retrying.
     *
     * The classification is here, and not in Ble, because here it is a pure
     * function of a Throwable: the host test constructs the real exception
     * types and checks the answer, with no phone and no network.
     *
     * The numbers mirror enum sync_net_fail in app/syncstat.h, which is where
     * they become an outcome; `make javacheck` compares the two lists.
     */
    static final int NET_OK = 0;
    static final int NET_DNS = 1;
    static final int NET_TIMEOUT = 2;
    static final int NET_TLS = 3;
    static final int NET_UNREACHABLE = 4;
    static final int NET_OTHER = 5;

    static int netFailure(Throwable t) {
        /* THE WHOLE CAUSE CHAIN. HttpURLConnection wraps freely -- an
         * UnknownHostException commonly arrives inside an IOException from
         * getOutputStream -- so classifying only the outermost throwable
         * reports OTHER for the failures that are easiest to name. */
        /* BOUNDED. A cause chain is supposed to end, but it is built by
         * whatever library threw, and a cycle here would hang the sync
         * worker -- which holds no lock and shows no progress, so it would
         * look exactly like a server that never answers. Eight links is far
         * past any real wrapping depth. */
        int depth = 0;
        for (Throwable c = t; c != null && depth < 8; c = c.getCause(), depth++) {
            /* TLS FIRST, and by its own types: every one of these is an
             * IOException too, so a test written against IOException would
             * report the certificate failures -- the ones a user can act on
             * -- as the generic kind. */
            if (c instanceof javax.net.ssl.SSLException
                || c instanceof java.security.cert.CertificateException
                || c instanceof java.security.GeneralSecurityException)
                return NET_TLS;
            if (c instanceof java.net.UnknownHostException)
                return NET_DNS;
            /* SocketTimeoutException is an InterruptedIOException, and so is
             * the timeout HttpURLConnection throws while writing a body. */
            if (c instanceof java.io.InterruptedIOException)
                return NET_TIMEOUT;
            if (c instanceof java.net.ConnectException
                || c instanceof java.net.NoRouteToHostException
                || c instanceof java.net.PortUnreachableException)
                return NET_UNREACHABLE;
        }
        return t == null ? NET_OK : NET_OTHER;
    }

    private NetPolicy() {}

    /* ---- THE FOREGROUND NOTIFICATION'S CONTENT, AS ONE VALUE ------------
     *
     * Title, text, value, lock state, pixels, width and height in seven
     * separate volatile fields, written one after another as each reading
     * lands and read one after another while the notification is built, are
     * individually safe and are not safe as a SET: two of the mixtures a
     * build can see are worse than stale:
     *
     *   - new pixels against the previous width and height. `px.length >= w *
     *     h` is the only thing standing between this and Bitmap.createBitmap
     *     reading past the end of the array, and a large new plot paired with
     *     small earlier dimensions PASSES it -- drawing a stretched corner of
     *     the new plot;
     *   - the value from one reading beside the title and text of another, so
     *     the status bar shows one number and the row beneath it another.
     *
     * The rule is here, and not only in PancraService, because it is pure: an
     * object either carries a plot whose dimensions match its pixels or it
     * carries none, and a call bringing no plot keeps the previous one. That is
     * three sentences of arithmetic guarding a read past the end of an array,
     * and on the host it can be executed rather than argued. */
}
