/* version.h -- build identity, printed as the first line of log.txt.
 *
 * A bug report is only actionable if it says which build produced it, and the
 * released binary has not always been the one running on a given device. Bump
 * CT_VERSION when cutting a release tag.
 *
 * build.sh may override CT_VERSION (-DCT_VERSION=...) for CI or throwaway
 * builds; the .git directory is not present inside the builder container, so
 * there is no commit hash to derive automatically.
 */

#ifndef __VERSION_H__
#define __VERSION_H__

#ifndef CT_VERSION
#define CT_VERSION "1.0.0-beta.8"
#endif

#endif
