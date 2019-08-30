#ifndef SCREENCAP_H
#define SCREENCAP_H

#ifdef __cplusplus
extern "C" {
#endif

int captureScreen(unsigned char** in_buf, int* out_size/*format_t type*/);

#ifdef __cplusplus
}
#endif


#endif // SCREENCAP_H
