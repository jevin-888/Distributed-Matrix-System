/*
 * DMS appliance policy: do not block the SSH listener on getrandom(2) while
 * the kernel CRNG is still initializing. Dropbear's existing /dev/urandom
 * path is non-blocking and S20urandom loads the persistent seed first.
 */
#undef HAVE_GETRANDOM
