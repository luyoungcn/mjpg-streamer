#ifndef SCREENCAP_H
#define SCREENCAP_H

#ifdef __cplusplus
extern "C" {
#endif

void* captureScreen_thread(void* args);
void* encode_thread(void* args);
int getEncodedBuf(unsigned char** in_buf, int* out_size/*format_t type*/);

#ifdef __cplusplus
}
#endif


#endif // SCREENCAP_H
