# Publishing to the AUR (not done yet)

The recipe here builds and was test-built locally on 2026-10-07. Still to do:

- [ ] Make the GitHub repo public (the AUR can't download private code).
- [ ] Create an account at https://aur.archlinux.org and add your SSH public key.
- [ ] Publish: `git clone ssh://aur@aur.archlinux.org/omasettings-git.git`, copy in
      `PKGBUILD` and `.SRCINFO`, commit, push.
- [ ] After any recipe change, refresh `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.
