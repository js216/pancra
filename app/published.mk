# THE LEDGER OF WHAT HAS ACTUALLY BEEN DISTRIBUTED.
# SPDX-License-Identifier: GPL-3.0
#
# WHY A CHECKED-IN FILE, AND NOT A NUMBER IN THE MAKEFILE OR IN A TAG.
#
# "The release version code must increase" is a rule about two DIFFERENT
# builds, and a build can only see itself. Whatever it compares against has
# to outlive the build that produced the previous artifact, survive a clean
# tree, a different machine and a different person, and be reviewable -- so
# it is a file in version control, changed by a human, in the same commit
# that records the release. A git tag would do for the number and not for
# the review; a value passed on the command line is exactly the thing that
# gets retyped wrong at 1am, which is the failure this exists to stop.
#
# WHAT THE RULE COSTS IF IT IS WRONG. Android refuses to install a package
# whose version code does not exceed the installed one. Ship two artifacts
# with the same code -- or a lower one -- and the second cannot update the
# first: every user is told the update failed, and the only way through is
# uninstall, which deletes this app's private storage. That storage is their
# glucose history. There is no recovery step after that, which is why this
# is checked before the artifact is declared good rather than discovered by
# somebody whose install failed.
#
# HOW TO RELEASE.
#   1. Set NEXT_VERSION_CODE to PUBLISHED_VERSION_CODE + 1 (below).
#   2. Build and publish: `make release ...` / `make aab`.
#   3. ONCE THE ARTIFACT IS ACTUALLY OUT -- uploaded, rolled out, in
#      somebody's hands -- set PUBLISHED_VERSION_CODE to the code that
#      shipped and bump NEXT_VERSION_CODE again, in one commit.
#   Step 3 is deliberately after distribution. A build that was made and
#   thrown away has published nothing, and recording it would burn a version
#   code for no artifact -- harmless, but it makes the ledger stop meaning
#   what it says.
#
# THE FIRST RELEASE. PUBLISHED_VERSION_CODE is 0 and nothing has ever been
# distributed, so there is no previous artifact to be unable to update and
# every code from 1 up satisfies the rule. 0 is not a real Android version
# code anybody shipped; it is the "no previous value" sentinel, and it is
# written down rather than left absent so the check can tell "nothing has
# been published" apart from "somebody deleted the line", which would
# otherwise both read as the empty string and compare true against anything.
PUBLISHED_VERSION_CODE := 0

# The code the next artifact gets when VERSION_CODE is not given on the
# command line. `make versioncheck` requires this to be exactly
# PUBLISHED_VERSION_CODE + 1, so the two lines cannot drift apart: a bumped
# PUBLISHED with a stale NEXT would otherwise default every subsequent build
# to a code that has already shipped.
NEXT_VERSION_CODE := 1
