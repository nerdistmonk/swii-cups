# swii-cups

A CUPS driver for Apple StyleWriter printers, for modern Linux.

This is a forked rewrite of lpstyl by Monroe Williams (<https://github.com/Godzil/lpstyl>). The original driver was written for NetBSD and other old Unix systems, and it needed obsolete system features and install paths. This version is rebuilt as a CUPS filter and backend, built with CMake and GCC.

Status: the code builds without warnings and the encoder test passes. Tested with CUPS 2.4.7 and ghostscript 10.02. It has not yet been tested with a real printer.

### Supported printers

Black-only models (`stylewriter-bw.ppd`):

* StyleWriter I
* StyleWriter II
* StyleWriter III (aka StyleWriter 1200)

Color models (`stylewriter-color.ppd`):

* Color StyleWriter 1500
* Color StyleWriter 2200
* Color StyleWriter 2400
* Color StyleWriter 2500

Black-only models never offer color. The color PPD has an `InkCartridge` option, so color models can print in black.

## How it works

The driver is two CUPS programs:

* `rastertostylewriter` is the filter. It reads CUPS raster data, renders it at 360 dpi, and writes a stream of frames to stdout. It never talks to the printer.
* `stylewriter` is the backend. It reads those frames and sends them to the printer over the serial port. It handles pacing, paper-out, and cancelling.

CUPS converts PostScript, PDF, and other formats to raster before the filter runs. You need the matching converters from cups-filters, such as `gstoraster` for PostScript and `pdftoraster` for PDF.

There are two PPDs, one for the black-only models and one for the color models. The `Model` option picks the printer model. The driver does not probe the printer for its model; the model comes from the option.

## Paper-out

When the printer reports that it is out of paper, the backend waits and checks again every 5 seconds, on all models. It logs a warning while it waits.

What happens depends on when the paper runs out:

* Right after the first band of a page is sent: black-only models reset the printer, wait 5 seconds, and resend the band. Color models wait, then resend the band.
* Later in the page, on a color model: the backend waits in place and resumes when paper is loaded.
* Later in the page, on a black-only model: the backend ejects the page and fails the job with an error.

A failed job follows the queue's error policy. The CUPS default, `stop-printer`, stops the queue. To discard the failed job and keep the queue running, set `printer-error-policy` to `abort-job`:

    sudo lpadmin -p SW2 -o printer-error-policy=abort-job

CUPS also cancels any job that runs longer than `MaxJobTime`, which defaults to 3 hours. To let a job wait longer for paper, set `MaxJobTime 0` in `/etc/cups/cupsd.conf` and restart CUPS. `MaxJobTime 0` disables the cancellation of stuck jobs.

## Pausing and cancelling

The original driver paused on `SIGUSR1`. That is gone. To hold the printer during a job, use CUPS: hold the job or disable the queue. Cancelling a job ejects the page if one is printing.

## Serial connection

The device URI is `stylewriter:` followed by an absolute path to the serial port, for example `stylewriter:/dev/ttyUSB0`.

The port opens at 57600 baud, 8N1. For the StyleWriter II, the driver switches the printer to 230400 baud before the page data. This follows the Mac driver's trace, but it has not been checked on hardware.

## Options

Set options with `lpadmin -o`, or in the CUPS web interface. For example:

    sudo lpadmin -p SW1500 -o InkCartridge=Black

* `Model` (both PPDs) sets the printer model. It decides the page margins, pacing, and protocol details. The PPD default is used unless you change it.
* `InkCartridge` (color PPD only) is `Black` (default) or `Color`. With `Black`, color pages print in black. Use it when only a black cartridge is installed.
* `Quality` (color PPD only) is `1` (Normal, default) or `2` (High). It maps to the original driver's `-q 2`.
* `NoMargins` (both PPDs) is `False` (default) or `True`. `True` skips the cropping that accounts for the top and left page margins, like the original `-m`. Use it only for files that don't take page margins into account, such as some Ghostscript samples.

## Building and installing

You need CMake, GCC, and the CUPS development files. On Debian or Ubuntu:

    sudo apt install cmake gcc libcups2-dev

On Arch Linux:

    sudo pacman -S --needed cmake gcc cups libcups

Build and run the tests:

    cmake -S . -B build
    cmake --build build
    cd build && ctest

Install the filter, the backend, and the PPDs:

    sudo cmake --install build

The filter goes in `<serverbin>/filter`, the backend in `<serverbin>/backend`, and the PPDs in `/usr/share/cups/model`. The default `<serverbin>` is `/usr/lib/cups`. Run `cups-config --serverbin` to see yours. If it differs, configure with `-DSWII_CUPS_SERVERBIN=<path>`, and use `-DSWII_CUPS_MODELDIR=<path>` for the PPDs.

Plug in the serial adapter. The backend lists USB serial devices (`/dev/ttyUSB*` and `/dev/ttyACM*`), so the printer should appear in `lpinfo -v` as `stylewriter:/dev/ttyUSB0`. It should also appear in the Add Printer dialog of your desktop, or at http://localhost:631. Choose the matching PPD there. To add it from the command line instead:

    sudo lpadmin -p SW2 -E -v stylewriter:/dev/ttyUSB0 -P /usr/share/cups/model/stylewriter-bw.ppd
    sudo lpadmin -p SW1500 -E -v stylewriter:/dev/ttyUSB0 -P /usr/share/cups/model/stylewriter-color.ppd

Use the device URI form `stylewriter:` followed by an absolute path. The examples use each PPD's default model. For a different model, pass it with `-o Model=<name>`, for example `-o Model=SW3`.

## Files

- `src/rastertostylewriter.c`: the CUPS filter. It reads CUPS raster data and writes frames to stdout.
- `src/stylewriter_backend.c`: the CUPS backend. It owns the serial port, pacing, and paper-out.
- `src/stylewriter.h`: the frame format shared by the filter and the backend.
- `src/stylewriter_encode.c`: the row encoder used by the filter.
- `tests/test_encode.c`: a round-trip test for the encoder, run by `ctest`.
- `stylewriter-bw.ppd`, `stylewriter-color.ppd`: PPDs for the black-only and color models.
- `README.protocol`: notes on the control protocol the printers use.
- `LICENSE`: the license.

## Copyright Notice

lpstyl was originally designed by Monroe Williams and his original lpstyl code is copyright 2000, Monrow Williams, all rights reserved.
All newly contributed code for the refactor/rewrite into swii-cups was designed by R.Shaklfrd and Claude Haiku.
