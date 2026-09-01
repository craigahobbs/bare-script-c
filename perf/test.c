/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The native C performance baseline
 *
 * The counterpart of the JavaScript implementation's perf/test.js and the Python implementation's
 * perf/test.py: the benchmark written directly in the host language, so "BareScript (C)" has a
 * floor to be measured against. Like those, it implements the tests it can and skips the rest.
 *
 * Only "mandelbrot" is implemented here, because only "mandelbrot" is the same work in every
 * language - the schema, markdown, QR code, and URL tests measure the BareScript include library's
 * own algorithms, which the other baselines compare against their host language's equivalent
 * libraries and C has none of.
 *
 *   perf-native [language [test]]
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


/* The performance tests and their iteration counts - the same counts the other implementations use */
static const struct {
    const char *name;
    int runs;
} perfTests[] = {
    {"mandelbrot", 1}
};

#define PERF_TEST_COUNT (sizeof(perfTests) / sizeof(perfTests[0]))


/*
 * The benchmark result sink
 *
 * The benchmark accumulates into this and main reads it, so the optimizer cannot delete the work
 * being measured.
 */
static volatile double perfSink;


static double perfNow(void)
{
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (double) now.tv_sec * 1000.0 + (double) now.tv_nsec / 1000000.0;
}


/* Compute one point of the Mandelbrot set */
static int mandelbrotValue(double xValue, double yValue, int maxIter)
{
    double c1r = xValue;
    double c1i = yValue;
    double c2r = 0;
    double c2i = 0;
    for (int iter = 1; iter <= maxIter; iter++) {
        if (sqrt(c2r * c2r + c2i * c2i) > 2) {
            return iter;
        }
        double c2rNew = c2r * c2r - c2i * c2i + c1r;
        c2i = 2 * c2r * c2i + c1i;
        c2r = c2rNew;
    }
    return 0;
}


/* Compute the "seahorse valley", a computationally dense region on the Mandelbrot set boundary */
static double mandelbrotSet(int width, int height, double xCoord, double yCoord, double xRange,
                            int maxIter)
{
    double yRange = ((double) height / width) * xRange;
    double xMin = xCoord - 0.5 * xRange;
    double yMin = yCoord - 0.5 * yRange;
    double total = 0;
    for (int ix = 0; ix < width; ix++) {
        for (int iy = 0; iy < height; iy++) {
            double xValue = xMin + ((double) ix / (width - 1)) * xRange;
            double yValue = yMin + ((double) iy / (height - 1)) * yRange;
            total += mandelbrotValue(xValue, yValue, maxIter);
        }
    }
    return total;
}


static double perfRun(int iterations)
{
    double timeBegin = perfNow();
    double total = 0;
    for (int ix = 0; ix < iterations; ix++) {
        total += mandelbrotSet(120, 80, -0.75, 0.1, 0.05, 60);
    }
    double timeMs = perfNow() - timeBegin;
    perfSink += total;
    return timeMs;
}


int main(int argc, char **argv)
{
    const char *language = argc > 1 ? argv[1] : "C";
    const char *testArg = argc > 2 ? argv[2] : NULL;

    for (size_t ix = 0; ix < PERF_TEST_COUNT; ix++) {
        /* Skip the tests the test argument filters out */
        if (testArg != NULL && strcmp(testArg, perfTests[ix].name) != 0) {
            continue;
        }
        double timeMs = perfRun(perfTests[ix].runs);
        printf("%s,%s,%d,%g\n", language, perfTests[ix].name, perfTests[ix].runs, timeMs);
    }

    /* Keep the accumulated result observable */
    if (perfSink < 0) {
        printf("%g\n", perfSink);
    }
    return 0;
}
