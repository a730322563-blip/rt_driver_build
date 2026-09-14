#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

class c_driver {
	private:
	int has_upper = 0;
	int has_lower = 0;
	int has_symbol = 0;
	int has_digit = 0;
	int fd;
	pid_t pid;

	typedef struct _COPY_MEMORY {
		pid_t pid;
		uintptr_t addr;
		void* buffer;
		size_t size;
	} COPY_MEMORY, *PCOPY_MEMORY;

	typedef struct _MODULE_BASE {
		pid_t pid;
		char* name;
		uintptr_t base;
	} MODULE_BASE, *PMODULE_BASE;

	enum OPERATIONS {
		OP_INIT_KEY = 0x800,
		OP_READ_MEM = 0x801,
		OP_WRITE_MEM = 0x802,
		OP_MODULE_BASE = 0x803,
		/* PTE 追踪 */
		OP_PTE_INSTALL = 0x810,
		OP_PTE_SET_TARGET = 0x811,
		OP_PTE_REMOVE = 0x812,
		OP_PTE_GET_HITS = 0x813,
		OP_PTE_SET_PAUSE = 0x814,
		/* 硬件断点追踪 */
		OP_HWBP_INSTALL = 0x820,
		OP_HWBP_SET_TARGET = 0x821,
		OP_HWBP_REMOVE = 0x822,
		OP_HWBP_GET_HITS = 0x823,
		OP_HWBP_SET_PAUSE = 0x824,
	};

	/* PTE 追踪请求结构 */
	typedef struct _PTE_INSTALL_REQ {
		pid_t pid;
		uint64_t addr;
		uint32_t active;
	} PTE_INSTALL_REQ;

	typedef struct _PTE_TARGET_REQ {
		uint64_t addr;
		uint32_t active;
		uint32_t fp_count;
		uint32_t fp_indices[8];
		uint64_t fp_values[8][2];
	} PTE_TARGET_REQ;
	
	int symbol_file(const char *filename) {
		//判断文件名是否含小写并且不含大写不含数字不含符号
		int length = strlen(filename);
		for (int i = 0; i < length; i++) {
			if (islower(filename[i])) {
				has_lower = 1;
			} else if (isupper(filename[i])) {
				has_upper = 1;
			} else if (ispunct(filename[i])) {
				has_symbol = 1;
			} else if (isdigit(filename[i])) {
				has_digit = 1;
			}
		}
		return has_lower && !has_upper && !has_symbol && !has_digit;
	}
	
	char *driver_path() {
	// 打开目录
		const char *dev_path = "/dev";
		DIR *dir = opendir(dev_path);
		if (dir == NULL){
			printf("无法打开/dev目录\n");
			return NULL;
		}

		struct dirent *entry;
		static char file_path[256];
		while ((entry = readdir(dir)) != NULL) {
			// 跳过当前目录和上级目录
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			sprintf(file_path, "%s/%s", dev_path, entry->d_name);
			// 获取文件stat结构
			struct stat file_info;
			if (stat(file_path, &file_info) < 0) {
				continue;
			}
			
			// 跳过gpio接口
			if (strstr(entry->d_name, "gpiochip") != NULL) {
				continue;
			}

			// 检查是否为驱动文件
			if ((S_ISCHR(file_info.st_mode) || S_ISBLK(file_info.st_mode))
				&& strchr(entry->d_name, '_') == NULL
				&& strchr(entry->d_name, '-') == NULL
				&& strchr(entry->d_name, ':') == NULL) {
				// 过滤标准输入输出
				if (strcmp(entry->d_name, "stdin") == 0
				|| strcmp(entry->d_name, "stdout") == 0
				|| strcmp(entry->d_name, "stderr") == 0) {
					continue;
				}
				
				size_t file_name_length = strlen(entry->d_name);
				time_t current_time;
				time(&current_time);
				int current_year = localtime(&current_time)->tm_year + 1900;
				int file_year = localtime(&file_info.st_ctime)->tm_year + 1900;
				//跳过1980年前的文件
				if (file_year <= 1980) {
					continue;
				}
				
				time_t atime = file_info.st_atime;
				time_t ctime = file_info.st_ctime;
				// 检查最近访问时间和修改时间是否一致并且文件名是否是symbol文件
				if ((atime == ctime)/* && symbol_file(entry->d_name)*/) {
					//检查mode权限类型是否为S_IFREG(普通文件)和大小还有gid和uid是否为0(root)并且文件名称长度在7位或7位以下
					if ((file_info.st_mode & S_IFMT) == 8192
					&& file_info.st_size == 0
					&& file_info.st_gid == 0
					&& file_info.st_uid == 0
					&& file_name_length <= 6) {
						closedir(dir);
						return file_path;
					}
				}
			}
		}
		closedir(dir);
		return NULL;
	}
	
	int open_driver() {
		char *dev_path3 = qx10();
		if (dev_path3 != NULL) {
			fd = open(dev_path3, O_RDWR);
			if (fd>0){
				printf("隐藏驱动：%s\n", dev_path3);
				unlink(dev_path3);
				return 1;
			}
		}
		char *dev_path1 = driver_path();
		if (dev_path1 != NULL) {
			fd = open(dev_path1, O_RDWR);
			if (fd>0){
				printf("驱动文件：%s\n", dev_path1);
				return 1;
			}
		}
		char *dev_path2 = qx8();
		if (dev_path2 != NULL) {
			fd = open(dev_path2, O_RDWR);
			if (fd>0){
				printf("隐藏驱动：%s\n", dev_path2);
				unlink(dev_path2);
				return 1;
			}
		}
		return 0;
	}
	
	public:
	c_driver() {
		open_driver();
		if (fd <= 0) {
			printf("[-] open driver failed\n");
			exit(0);
		}
	}

	~c_driver() {
		//wont be called
		if (fd > 0)
			close(fd);
	}

	void initialize(pid_t pid) {
		this->pid = pid;
	}

	bool init_key(char* key) {
		char buf[0x100];
		strcpy(buf,key);
		if (ioctl(fd, OP_INIT_KEY, buf) != 0) {
			return false;
		}
		return true;
	}

	bool read(uintptr_t addr, void *buffer, size_t size) {
		COPY_MEMORY cm;

		cm.pid = this->pid;
		cm.addr = addr;
		cm.buffer = buffer;
		cm.size = size;

		if (ioctl(fd, OP_READ_MEM, &cm) != 0) {
			return false;
		}
		return true;
	}

	bool write(uintptr_t addr, void *buffer, size_t size) {
		COPY_MEMORY cm;

		cm.pid = this->pid;
		cm.addr = addr;
		cm.buffer = buffer;
		cm.size = size;

		if (ioctl(fd, OP_WRITE_MEM, &cm) != 0) {
			return false;
		}
		return true;
	}

	template <typename T>
	T read(uintptr_t addr) {
		T res;
		if (this->read(addr, &res, sizeof(T)))
			return res;
		return {};
	}

	template <typename T>
	bool write(uintptr_t addr,T value) {
		return this->write(addr, &value, sizeof(T));
	}

	uintptr_t get_module_base(char* name) {
		MODULE_BASE mb;
		char buf[0x100];
		strcpy(buf,name);
		mb.pid = this->pid;
		mb.name = buf;

		if (ioctl(fd, OP_MODULE_BASE, &mb) != 0) {
			return 0;
		}
		return mb.base;
	}

	/* ===== PTE 追踪接口 ===== */
	bool pte_install(pid_t target_pid, uint64_t target_addr) {
		PTE_INSTALL_REQ req;
		req.pid = target_pid;
		req.addr = target_addr;
		req.active = 0;
		return ioctl(fd, OP_PTE_INSTALL, &req) == 0;
	}

	bool pte_set_target(float x, float y, float z) {
		PTE_TARGET_REQ req;
		memset(&req, 0, sizeof(req));
		req.active = 1;
		req.fp_count = 3;
		req.fp_indices[0] = 3; /* V3 = x */
		req.fp_indices[1] = 4; /* V4 = y */
		req.fp_indices[2] = 5; /* V5 = z */
		uint32_t fx, fy, fz;
		memcpy(&fx, &x, 4);
		memcpy(&fy, &y, 4);
		memcpy(&fz, &z, 4);
		req.fp_values[0][0] = fx;
		req.fp_values[1][0] = fy;
		req.fp_values[2][0] = fz;
		return ioctl(fd, OP_PTE_SET_TARGET, &req) == 0;
	}

	bool pte_set_target_raw(uint32_t fp_count, uint32_t* indices, uint64_t (*values)[2]) {
		PTE_TARGET_REQ req;
		memset(&req, 0, sizeof(req));
		req.active = 1;
		req.fp_count = fp_count > 8 ? 8 : fp_count;
		for (uint32_t i = 0; i < req.fp_count; i++) {
			req.fp_indices[i] = indices[i];
			req.fp_values[i][0] = values[i][0];
			req.fp_values[i][1] = values[i][1];
		}
		return ioctl(fd, OP_PTE_SET_TARGET, &req) == 0;
	}

	void pte_remove() {
		ioctl(fd, OP_PTE_REMOVE, 0);
	}

	uint64_t pte_get_hits() {
		uint64_t hits = 0;
		ioctl(fd, OP_PTE_GET_HITS, &hits);
		return hits;
	}

	void pte_pause() {
		int p = 1;
		ioctl(fd, OP_PTE_SET_PAUSE, &p);
	}

	void pte_resume() {
		int p = 0;
		ioctl(fd, OP_PTE_SET_PAUSE, &p);
	}

	/* ===== 硬件断点追踪接口 ===== */
	bool hwbp_install(pid_t target_pid, uint64_t target_addr) {
		struct {
			pid_t pid;
			uint64_t addr;
			uint32_t active;
		} req;
		req.pid = target_pid;
		req.addr = target_addr;
		req.active = 0;
		return ioctl(fd, OP_HWBP_INSTALL, &req) == 0;
	}

	bool hwbp_set_target(float x, float y, float z) {
		struct {
			uint64_t addr;
			uint32_t active;
			uint32_t fp_count;
			uint32_t fp_indices[8];
			uint64_t fp_values[8][2];
		} req;
		memset(&req, 0, sizeof(req));
		req.active = 1;
		req.fp_count = 3;
		req.fp_indices[0] = 3; /* V3 = x */
		req.fp_indices[1] = 4; /* V4 = y */
		req.fp_indices[2] = 5; /* V5 = z */
		uint32_t fx, fy, fz;
		memcpy(&fx, &x, 4);
		memcpy(&fy, &y, 4);
		memcpy(&fz, &z, 4);
		req.fp_values[0][0] = fx;
		req.fp_values[1][0] = fy;
		req.fp_values[2][0] = fz;
		return ioctl(fd, OP_HWBP_SET_TARGET, &req) == 0;
	}

	void hwbp_remove() {
		ioctl(fd, OP_HWBP_REMOVE, 0);
	}

	uint64_t hwbp_get_hits() {
		uint64_t hits = 0;
		ioctl(fd, OP_HWBP_GET_HITS, &hits);
		return hits;
	}

	void hwbp_pause() {
		int p = 1;
		ioctl(fd, OP_HWBP_SET_PAUSE, &p);
	}

	void hwbp_resume() {
		int p = 0;
		ioctl(fd, OP_HWBP_SET_PAUSE, &p);
	}
};

static c_driver *driver = new c_driver();

/*--------------------------------------------------------------------------------------------------------*/

typedef char PACKAGENAME;	// 包名
pid_t pid;	// 进程ID

float Kernel_v()
{
	const char* command = "uname -r | sed 's/\\.[^.]*$//g'";
	FILE* file = popen(command, "r");
	if (file == NULL) {
    	return NULL;
	}
	static char result[512];
	if (fgets(result, sizeof(result), file) == NULL) {
		return NULL;
	}
	pclose(file);
    result[strlen(result)-1] = '\0';
	return atof(result);
}

char *GetVersion(char* PackageName)
{
	char command[256];
	sprintf(command, "dumpsys package %s|grep versionName|sed 's/=/\\n/g'|tail -n 1", PackageName);
	FILE* file = popen(command, "r");
	if (file == NULL) {
		return NULL;
	}
	static char result[512];
	if (fgets(result, sizeof(result), file) == NULL) {
		return NULL;
	}
	pclose(file);
	result[strlen(result)-1] = '\0';
	return result;
}

uint64_t GetTime()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC,&ts);
	return (ts.tv_sec*1000 + ts.tv_nsec/(1000*1000));
}

char *getDirectory()
{
	static char buf[128];
	int rslt = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (rslt < 0 || (rslt >= sizeof(buf) - 1))
	{
		return NULL;
	}
	buf[rslt] = '\0';
	for (int i = rslt; i >= 0; i--)
	{
		if (buf[i] == '/')
		{
			buf[i] = '\0';
			break;
		}
	}
	return buf;
}

int getPID(char* PackageName)
{
	FILE* fp;
    char cmd[0x100] = "pidof ";
    strcat(cmd, PackageName);
    fp = popen(cmd,"r");
    fscanf(fp,"%d", &pid);
    pclose(fp);
	if (pid > 0)
	{
		driver->initialize(pid);
	}
    return pid;
}

bool PidExamIne()
{
	char path[128];
	sprintf(path, "/proc/%d",pid);
	if (access(path,F_OK) != 0)
	{
		printf("\033[31;1m");
		puts("获取进程PID失败!");
		exit(1);
	}
	return true;
}

long GetModuleBaseAddr(char* module_name)
{
    long addr = 0;
    char filename[32];
    char line[1024];
    if (pid < 0)
    {
        snprintf(filename, sizeof(filename), "/proc/self/maps", pid);
    }
    else
    {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    FILE *fp = fopen(filename, "r");
    if (fp != NULL)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, module_name))
            {
				sscanf(line,"%lx-%*lx",&addr);
                break;
            }
        }
        fclose(fp);
    }
    return addr;
}

long getModuleBase(char* module_name)
{
	uintptr_t base=0;
	if (Kernel_v() >= 6.0)
		base = GetModuleBaseAddr(module_name);
	else
		base = driver->get_module_base(module_name);
	return base;
}

long ReadValue(long addr)
{
	long he=0;
	if (addr < 0xFFFFFFFF){
		driver->read(addr, &he, 4);
	}else{
		driver->read(addr, &he, 8);
		he=he&0xFFFFFFFFFFFF;
	}
	return he;
}

long ReadDword(long addr)
{
	long he=0;
	driver->read(addr, &he, 4);
	return he;
}

float ReadFloat(long addr)
{
	float he=0;
	driver->read(addr, &he, 4);
	return he;
}

int *ReadArray(long addr)
{
	int *he = (int *) malloc(12);
	driver->read(addr, he, 12);
	return he;
}

int WriteDword(long int addr, int value)
{
	driver->write(addr, &value, 4);
	return 0;
}

int WriteFloat(long int addr, float value)
{
	driver->write(addr, &value, 4);
	return 0;
}