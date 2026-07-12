# Third-Party Components

This repository does not include the VMX-pi HAL binary package.

The VMX-pi HAL, headers and related software are provided by Kauai Labs and
remain subject to their respective copyright and license terms.

The installer accepts a separately obtained ARMHF HAL package with:

    sudo ./install.sh --hal-deb /path/to/vmxpi-hal_armhf.deb

## HAL download source

The default installer configuration downloads the separately distributed
VMX-pi HAL 1.1.257 ARMHF package from:

    https://archive.org/download/vmxpi-hal_1.1.257_armhf/vmxpi-hal_1.1.257_armhf.deb

This archive download is not part of this Git repository. The downloaded
package remains subject to the copyright and license terms of its provider
and original authors. The download URL can be overridden with --hal-url,
or a local package can be supplied with --hal-deb.


The expected SHA256 checksum for the default archived package is:

    276b608248545d2c244e512650291ca5b36da16741aada7892e45a641697cc45
