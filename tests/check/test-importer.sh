# Paired payload coverage, using the same package builder as the compose tests.
set -euo pipefail
. "${commondir}/libtest.sh"
formats=(4)
if test -n "$(rpm --eval '%{?_rpmformat}')"; then
    formats+=(6)
else
    echo 'ok # SKIP rpmbuild cannot build v6 packages (requires RPM 6)'
fi
for format in "${formats[@]}"; do
    build_rpm payload-test repository no rpmformat "$format" install '
mkdir -p %{buildroot}/usr/share/payload-test/shared %{buildroot}/etc
printf "hardlinked payload\n" > %{buildroot}/usr/share/payload-test/a
ln %{buildroot}/usr/share/payload-test/a %{buildroot}/usr/share/payload-test/b
ln %{buildroot}/usr/share/payload-test/a %{buildroot}/usr/share/payload-test/c
printf "non-ASCII filename\n" > %{buildroot}/usr/share/payload-test/café
printf "first owner\n" > %{buildroot}/usr/share/payload-test/owner-one
printf "second owner\n" > %{buildroot}/usr/share/payload-test/owner-two
touch %{buildroot}/usr/share/payload-test/empty
ln %{buildroot}/usr/share/payload-test/empty %{buildroot}/usr/share/payload-test/empty-link
ln -s a %{buildroot}/usr/share/payload-test/link
dd if=/dev/zero of=%{buildroot}/usr/share/payload-test/large bs=1M count=2
echo configuration > %{buildroot}/etc/payload-test
' files '
%dir %attr(0750,root,root) /usr/share/payload-test
%doc %attr(0640,root,root) /usr/share/payload-test/a
%attr(0640,root,root) /usr/share/payload-test/b
%doc %attr(0640,root,root) /usr/share/payload-test/c
%attr(0640,root,root) /usr/share/payload-test/café
%attr(0640,root,root) /usr/share/payload-test/empty*
%attr(0640,root,root) /usr/share/payload-test/link
%attr(0640,root,root) /usr/share/payload-test/large
%attr(4750,payload-user-one,payload-group-one) /usr/share/payload-test/owner-one
%attr(2750,payload-user-two,payload-group-two) /usr/share/payload-test/owner-two
%dir %attr(2770,root,payload-group-two) /usr/share/payload-test/shared
%config(noreplace) /etc/payload-test
%ghost /usr/share/payload-test/ghost
%caps(cap_net_bind_service=ep) /usr/bin/payload-test
'
    mv yumrepo/packages/x86_64/payload-test-1.0-1.x86_64.rpm "payload-v${format}.rpm"
    if test "${#formats[@]}" = 2; then
        assert_streq "$(rpm -qp --qf '%{RPMFORMAT}' "payload-v${format}.rpm")" "$format"
    fi
done
export RPMOSTREE_TEST_RPM_V4="$PWD/payload-v4.rpm"
if test "${#formats[@]}" = 2; then
    export RPMOSTREE_TEST_RPM_V6="$PWD/payload-v6.rpm"
fi
LC_ALL=C rpm-ostree testutils c-units
