#include <compsky/macros/likely.hpp>


extern "C"
int init_all();

extern "C"
int uninit_all();

extern "C"
void playAudio(const char* const filePath);


int main(const int argc,  const char* const* const argv){
	if (unlikely(init_all()))
		return 1;
	for (int i = 1;  i < argc;  ++i){
		playAudio(argv[i]);
	}
	uninit_all();
	return 0;
}
