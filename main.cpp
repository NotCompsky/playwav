#include <cstdint> // for a2f.hpp (temporary; already fixed libcompsky)
#include <compsky/macros/likely.hpp>
#include <compsky/deasciify/a2f.hpp>
#include <compsky/deasciify/a2n.hpp>
#include <unistd.h>


struct GlobalVars;

extern "C"
GlobalVars* initFFMPEG();

extern "C"
void uninitFFMPEG(GlobalVars* const globalvars);

extern "C"
void playAudio(GlobalVars* const globalvars,  const char* const filePath,  float,  float,  float);


int main(const int argc,  const char* const* const argv){
	if (unlikely(argc == 1)){
		write(1, "USAGE: [-l, to loop everything] [-r REPEATS=1, to repeat the next audio] [-v VOLUME=1.0] [[FILEPATHS]]\n", 103);
		return 0;
	}
	GlobalVars* const globalvars = initFFMPEG();
	if (unlikely(globalvars == nullptr))
		return 1;
	bool loop = false;
	float volume = 1.0;
	unsigned n_repeats = 1;
	do {
		for (int i = 1;  i < argc;  ++i){
			if (argv[i][0] == '-'){
				if (argv[i][2] == 0){
					if (argv[i][1] == 'l'){
						loop = true;
						continue;
					}
					if (argv[i][1] == 'v'){
						++i;
						if (i < argc){
							volume = a2f<float,const char*,false>(argv[i]);
							continue;
						}
					}
					if (argv[i][1] == 'r'){
						++i;
						if (i < argc){
							n_repeats = a2n<unsigned,const char*,false>(argv[i]);
							continue;
						}
					}
				}
			}
			do {
				playAudio(globalvars, argv[i], 0.0, 0.0, volume);
			} while(--n_repeats != 0);
			++n_repeats;
		}
	} while(loop);
	uninitFFMPEG(globalvars);
	return 0;
}
