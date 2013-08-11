// adoftp.c
// anonymous download-only FTP server
//
// compile on Solaris/SunOS:
//   cc -lsocket -lnsl -o adoftp adoftp.c
// compile on Linux:
//   cc -lpthread -o adoftp adoftp.c
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define WRITE_BUFFER_SIZE 256
#define BUFFER_SIZE 4096
#define FILE_READ_BUFFER_SIZE 4096

#define CONN_MODE_ACTIVE 1
#define CONN_MODE_PASSIVE 2

// data for connected client, every thread has one instance of this struct
typedef struct
{
	int fd;
	char buf[BUFFER_SIZE];
	int buffer_pos;

	int data_connection_mode;

	struct sockaddr_in active_addr;
	int active_fd;

	int passive_fd;
	int passive_client_fd;

	char dir[PATH_MAX + 1];
	int binary_flag;
} CLIENT_INFO;

// base directory
char basedir[PATH_MAX + 1] = { 0 };

// prints out an error message and exits the program
void epicfail(char * msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

// creates a TCP server socket, binds it to the specified address and port and starts listening
int create_tcp_server_socket(char * addr, int port)
{
	int sock;
	if ((sock = socket(PF_INET, SOCK_STREAM, 6 /* TCP */)) == -1) epicfail("socket");

	struct sockaddr_in in;
	bzero(&in, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_port = htons(port);
	inet_pton(AF_INET, addr, &(in.sin_addr));

	if (bind(sock, (struct sockaddr *)&in, sizeof(in)) == -1) epicfail("bind");

	if (listen(sock, 0) == -1) epicfail("listen");
	
	return sock;
}

// waits for a connection to be made for the specified socket
int accept_connection(int fd)
{
	struct sockaddr_in ca;
	socklen_t sz = sizeof(ca);
	int client;
	if ((client = accept(fd, (struct sockaddr *)&ca, &sz)) == -1) epicfail("accept");

	return client;
}

// writes a null-terminated string into the file descriptor
void write_string(int fd, char * s)
{
	int len = strlen(s);
	int bytes_written = write(fd, s, len);
	if (bytes_written != len) epicfail("write"); // TODO
}

// sends an FTP status code with a message
void send_code_param(int fd, int code, char * p1)
{
	char buf[WRITE_BUFFER_SIZE] = { 0 };

	if (code == 150) strncpy(buf, "150 Opening connection", WRITE_BUFFER_SIZE - 1);
	else if (code == 200) strncpy(buf, "200 Okay", WRITE_BUFFER_SIZE - 1);
	else if (code == 215) snprintf(buf, WRITE_BUFFER_SIZE - 1, "215 %s", p1);
	else if (code == 220) strncpy(buf, "220 Service ready", WRITE_BUFFER_SIZE - 1);
	else if (code == 221) strncpy(buf, "221 Goodbye", WRITE_BUFFER_SIZE - 1);
	else if (code == 226) strncpy(buf, "226 Transfer complete", WRITE_BUFFER_SIZE - 1);
	else if (code == 227) snprintf(buf, WRITE_BUFFER_SIZE - 1, "227 Entering Passive Mode (%s).", p1);
	else if (code == 230) strncpy(buf, "230 User logged in", WRITE_BUFFER_SIZE - 1);
	else if (code == 250) strncpy(buf, "250 Command successful", WRITE_BUFFER_SIZE - 1);
	else if (code == 257) snprintf(buf, WRITE_BUFFER_SIZE - 1, "257 \"%s\"\r\n", p1);
	else if (code == 331) strncpy(buf, "331 User name ok, need password", WRITE_BUFFER_SIZE - 1);
	else if (code == 500) strncpy(buf, "500 Syntax error, command unrecognized", WRITE_BUFFER_SIZE - 1);
	else if (code == 550) strncpy(buf, "550 Requested action not taken.", WRITE_BUFFER_SIZE - 1);
	else epicfail("Invalid code.");

	strncat(buf, "\r\n", WRITE_BUFFER_SIZE - 1);

	int len = strlen(buf);
	int bytes_written = write(fd, buf, len);
	if (bytes_written != len) epicfail("write"); // TODO
}

// sends an FTP status code with a message
void send_code(int fd, int code)
{
	send_code_param(fd, code, NULL);
}

// returns 1 if the buffer starts with the specified command
int compare_command(char * buf, char * command)
{
	return memcmp(buf, command, strlen(command)) == 0;
}

// perform a read operation from the client
int client_read(int fd, char * buf, int * buffer_pos)
{
	if (BUFFER_SIZE - *buffer_pos - 1 == 0) epicfail("buffer full");

	int bytes_read = read(fd, buf + *buffer_pos, BUFFER_SIZE - *buffer_pos - 1);
	if (bytes_read == 0) return 0;
	if (bytes_read == -1) return -1;
	*buffer_pos += bytes_read;

	return bytes_read;
}

// read from the client until there is a whole line in the buffer
int client_read_line(int fd, char * buf, int * buffer_pos)
{
	while ((strstr(buf, "\r\n") == NULL) && (strstr(buf, "\n") == NULL))
	{
		int res = client_read(fd, buf, buffer_pos);
		if (res == -1) return -1;
	}

	return 0;
}

// extract and remove a line from the buffer
void extract_line(char * line, char * buf, int * buffer_pos)
{
	char * crlf_pos = strstr(buf, "\r\n");
	int crlf_len = 2;
	if (crlf_pos == NULL)
	{
		crlf_pos = strstr(buf, "\n");
		crlf_len = 1;
	}

	if (crlf_pos == NULL) epicfail("No newline in buffer");

	strncpy(line, buf, crlf_pos - buf);
	int new_len = *buffer_pos - (crlf_pos - buf) - crlf_len;
	memmove(buf, crlf_pos + crlf_len, new_len);
	*buffer_pos = new_len;
}

// perform FTP USER command, take any username as valid
void command_user(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len < 6)
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code(client_info->fd, 331);
}

// perform FTP PASS command, take any password as valid
void command_pass(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len < 5)
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code(client_info->fd, 230);
}

// perform FTP NOOP command
void command_noop(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len != 4)
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code(client_info->fd, 200);
}

// perform FTP SYST command, identify as a standard UNIX FTP server
void command_syst(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len != 4)
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code_param(client_info->fd, 215, "UNIX Type: L8");
}

// perform FTP TYPE command, switch binary mode on and off
void command_type(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len < 6)
	{
		send_code(client_info->fd, 500);
		return;
	}

	char * param = line + 5;
	if (strcmp(param, "A") == 0) client_info->binary_flag = 0;
	else if (strcmp(param, "A N") == 0) client_info->binary_flag = 0;
	else if (strcmp(param, "I") == 0) client_info->binary_flag = 1;
	else if (strcmp(param, "L 8") == 0) client_info->binary_flag = 1;
	else
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code(client_info->fd, 200);
}

// perform FTP PWD command, prints current directory
void command_pwd(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len != 3)
	{
		send_code(client_info->fd, 500);
		return;
	}

	send_code_param(client_info->fd, 257, client_info->dir);
}

// perform FTP PORT command, prepare for active data connection
void command_port(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);

	int ip1, ip2, ip3, ip4, port1, port2;
	if (sscanf(line + 5, "%d,%d,%d,%d,%d,%d", &ip1, &ip2, &ip3, &ip4, &port1, &port2) != 6)
	{
		send_code(client_info->fd, 500);
		return;
	}

	client_info->data_connection_mode = CONN_MODE_ACTIVE;

	memset(&(client_info->active_addr), 0, sizeof(client_info->active_addr));
	client_info->active_addr.sin_family = AF_INET;
	char buf_addr[32];
	sprintf(buf_addr, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
	inet_pton(AF_INET, buf_addr, &(client_info->active_addr.sin_addr));
	client_info->active_addr.sin_port = htons((port1 << 8) + port2);

	send_code(client_info->fd, 200);
}

// perform FTP PASV command, prepare for passive data connection
void command_pasv(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len != 4)
	{
		send_code(client_info->fd, 500);
		return;
	}

	if (client_info->passive_fd != 0) 
	{
		close(client_info->passive_fd);
		client_info->passive_fd = 0;
	}

	struct sockaddr_in s;
	socklen_t l;

	l = sizeof(s);
	getsockname(client_info->fd, (struct sockaddr *)&s, &l);
	char * ip = inet_ntoa(s.sin_addr);
	if (! ip) epicfail("inet_ntoa");

	client_info->data_connection_mode = CONN_MODE_PASSIVE;
	client_info->passive_fd = create_tcp_server_socket(ip, 0);

	l = sizeof(s);
	getsockname(client_info->passive_fd, (struct sockaddr *)&s, &l);
	int port = ntohs(s.sin_port);

	int ip1, ip2, ip3, ip4, port1, port2;
	if (sscanf(ip, "%d.%d.%d.%d", &ip1, &ip2, &ip3, &ip4) != 4) epicfail("command_pasv");
	port1 = port >> 8;
	port2 = port & 0xff;

	char p[64];
	sprintf(p, "%d,%d,%d,%d,%d,%d", ip1, ip2, ip3, ip4, port1, port2);

	send_code_param(client_info->fd, 227, p);
}

// returns a letter representing the file type (specified by a mode_t)
// stolen from gnulib, lib/lib_filemode.c
static char ftypelet (mode_t bits)
{
  if (S_ISREG (bits)) return '-';
  if (S_ISDIR (bits)) return 'd';
  if (S_ISBLK (bits)) return 'b';
  if (S_ISCHR (bits)) return 'c';
  if (S_ISLNK (bits)) return 'l';
  if (S_ISFIFO (bits)) return 'p';
  if (S_ISSOCK (bits)) return 's';
  return '?';
}

// opens a data connection with the client (either passive or active)
void open_data_connection(CLIENT_INFO * client_info)
{
	if (client_info->data_connection_mode == CONN_MODE_ACTIVE)
	{
		client_info->active_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (client_info->active_fd < 0) epicfail("socket");

		if (connect(client_info->active_fd, (struct sockaddr *)&client_info->active_addr, sizeof(client_info->active_addr)) < 0) epicfail("connect"); // TODO
	}
	else if (client_info->data_connection_mode == CONN_MODE_PASSIVE)
	{
		client_info->passive_client_fd = accept_connection(client_info->passive_fd); // TODO
		close(client_info->passive_fd);
		client_info->passive_fd = 0;
	}
	else
	{
		epicfail("open_data_connection");
	}
}

// closes the data connection to the client
void close_data_connection(CLIENT_INFO * client_info)
{
	if (client_info->data_connection_mode == CONN_MODE_ACTIVE)
	{
		close(client_info->active_fd);
		client_info->active_fd = 0;
	}
	else
	{
		close(client_info->passive_client_fd);
		client_info->passive_client_fd = 0;
	}
}

// sends a string over the data connection
void data_connection_write_string(CLIENT_INFO * client_info, char * str)
{
	if (client_info->data_connection_mode == CONN_MODE_ACTIVE) write_string(client_info->active_fd, str);
	else if (client_info->data_connection_mode == CONN_MODE_PASSIVE) write_string(client_info->passive_client_fd, str);
	else epicfail("data_connection_write_string");
}

// sends a buffer over the data connection
int data_connection_write_buffer(CLIENT_INFO * client_info, char * buf, int len)
{
	int bytes_written = 0;

	if (client_info->data_connection_mode == CONN_MODE_ACTIVE) bytes_written = write(client_info->active_fd, buf, len);
	else if (client_info->data_connection_mode == CONN_MODE_PASSIVE) bytes_written = write(client_info->passive_client_fd, buf, len);
	else epicfail("data_connection_write_buffer");

	if (len != bytes_written) return -1;

	return 0;
}

// perform FTP LIST command, sends a directory listing to the client
void command_list(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);

	char * path = NULL;
	if (len != 4)
	{
		char * p = line + 5;
		if (*p == '-')
		{
			while (*p && (*p != ' ')) p++;
		}

		while (*p && (*p == ' ')) p++;
		path = p;
	}

	char dirbuf[PATH_MAX + 1] = { 0 };
	snprintf(dirbuf, PATH_MAX, "%s%s", basedir, client_info->dir);

	if (path)
	{
		if (path[0] == '/')
		{
			snprintf(dirbuf, PATH_MAX, "%s%s", basedir, path);
		}
		else
		{
			strcat(dirbuf, "/");
			strcat(dirbuf, path);
		}
	}

	char realpathbuf[PATH_MAX + 1] = { 0 };
	if (! realpath(dirbuf, realpathbuf))
	{
		send_code(client_info->fd, 550);
		return;
	}

	strcpy(dirbuf, realpathbuf);

	if (dirbuf[strlen(dirbuf) - 1] != '/')
		strncat(dirbuf, "/", PATH_MAX);

	DIR * dirp = opendir(dirbuf);
	if (! dirp)
	{
		send_code(client_info->fd, 550);
		return;
	}

	send_code(client_info->fd, 150);
	open_data_connection(client_info);

	while (1)
	{
		struct dirent * entry = readdir(dirp);
		if (! entry) break;

		char filenamebuf[PATH_MAX + 1] = { 0 };
		snprintf(filenamebuf, PATH_MAX, "%s%s", dirbuf, entry->d_name);

		struct stat s;
		if (stat(filenamebuf, &s) == -1)
		{
			// cannot stat
			continue;
		}

		char buf[PATH_MAX + 128 + 1] = { 0 };
		strmode(s.st_mode, buf);
		unsigned int size = (unsigned int)s.st_size;
		char date[64];
		struct tm * ts = localtime(&s.st_mtime);
		strftime(date, 64, "%b %e  %Y", ts);
		sprintf(buf + 11, "%3d %-8d %-8d %8u %s %s\r\n", s.st_nlink, (int)s.st_uid, (int)s.st_gid, size, date, entry->d_name);
		data_connection_write_string(client_info, buf);
	}

	close_data_connection(client_info);

	send_code(client_info->fd, 226);
}

// perform FTP CWD command, changes directory
void command_cwd(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len < 5)
	{
		send_code(client_info->fd, 500);
		return;
	}

	char dirbackup[PATH_MAX + 1] = { 0 };
	strncpy(dirbackup, client_info->dir, PATH_MAX);

	char * dir = line + 4;
	if (dir[0] == '/')
	{
		strncpy(client_info->dir, dir, PATH_MAX);
	}
	else
	{
		strncat(client_info->dir, dir, PATH_MAX);
		strncat(client_info->dir, "/", PATH_MAX);
	}

	char dirbuf[PATH_MAX + 1] = { 0 };
	snprintf(dirbuf, PATH_MAX, "%s%s", basedir, client_info->dir);

	char pathbuf[PATH_MAX + 1] = { 0 };
	if (! realpath(dirbuf, pathbuf))
	{
		strncpy(client_info->dir, dirbackup, PATH_MAX);
		send_code(client_info->fd, 550);
		return;
	}

	strncpy(client_info->dir, pathbuf, PATH_MAX);

	if (strlen(client_info->dir) < strlen(basedir))
	{
		strncpy(client_info->dir, dirbackup, PATH_MAX);
		send_code(client_info->fd, 550);
		return;
	}

	memmove(client_info->dir, client_info->dir + strlen(basedir), strlen(client_info->dir) - strlen(basedir) + 1);

	if (client_info->dir[strlen(client_info->dir) - 1] != '/')
		strncat(client_info->dir, "/", PATH_MAX);

	send_code(client_info->fd, 250);
}

// perform FTP RETR command, sends a file to the client
void command_retr(CLIENT_INFO * client_info)
{
	char line[BUFFER_SIZE] = { 0 };
	client_read_line(client_info->fd, client_info->buf, &client_info->buffer_pos);
	extract_line(line, client_info->buf, &client_info->buffer_pos);
	int len = strlen(line);
	if (len < 6) 
	{
		send_code(client_info->fd, 500);
		return;
	}

	char * filename = line + 5;
	char filenamebuf[PATH_MAX + 1] = { 0 };

	if (filename[0] == '/')
	{
		snprintf(filenamebuf, PATH_MAX, "%s%s", basedir, filename);
	}
	else
	{
		snprintf(filenamebuf, PATH_MAX, "%s%s%s", basedir, client_info->dir, filename);
	}

	int fd = open(filenamebuf, O_RDONLY);
	if (fd == -1)
	{
		// cannot open file
		send_code(client_info->fd, 550);
		return;
	}

	send_code(client_info->fd, 150);
	open_data_connection(client_info);

	while (1)
	{
		char buf[FILE_READ_BUFFER_SIZE];
		int bytes_read = read(fd, buf, FILE_READ_BUFFER_SIZE);
		if (bytes_read == 0) break;
		if (bytes_read == -1) epicfail("read");

		int res = data_connection_write_buffer(client_info, buf, bytes_read);
		if (res == -1) break;
	}

	close(fd);

	close_data_connection(client_info);

	send_code(client_info->fd, 226);
}

// reads a line from the client buffer
void get_line(int fd, char * line, char * buf, int * buffer_pos)
{
	client_read_line(fd, buf, buffer_pos);
	extract_line(line, buf, buffer_pos);
}

// main client handler procedure, runs in its own thread
void * thread_proc(void * param)
{
	CLIENT_INFO client_info;
	memset(&client_info, 0, sizeof(client_info));
	client_info.fd = (int)param;
	strcpy(client_info.dir, "/");

	send_code(client_info.fd, 220);

	while (1)
	{
		int result = client_read(client_info.fd, client_info.buf, &client_info.buffer_pos);
		if (result == -1) break;

		while (client_info.buffer_pos >= 4)
		{
			char line[BUFFER_SIZE] = { 0 };

			if (compare_command(client_info.buf, "USER")) command_user(&client_info);
			else if (compare_command(client_info.buf, "PASS")) command_pass(&client_info);
			else if (compare_command(client_info.buf, "PWD")) command_pwd(&client_info);
			else if (compare_command(client_info.buf, "PORT")) command_port(&client_info);
			else if (compare_command(client_info.buf, "PASV")) command_pasv(&client_info);
			else if (compare_command(client_info.buf, "LIST")) command_list(&client_info);
			else if (compare_command(client_info.buf, "CWD")) command_cwd(&client_info);
			else if (compare_command(client_info.buf, "RETR")) command_retr(&client_info);
			else if (compare_command(client_info.buf, "NOOP")) command_noop(&client_info);
			else if (compare_command(client_info.buf, "SYST")) command_syst(&client_info);
			else if (compare_command(client_info.buf, "TYPE")) command_type(&client_info);
			else if (compare_command(client_info.buf, "QUIT"))
			{
				get_line(client_info.fd, line, client_info.buf, &client_info.buffer_pos);
				send_code(client_info.fd, 221);
				close(client_info.fd);
				client_info.fd = 0;
				return NULL;
			}
			else
			{
				get_line(client_info.fd, line, client_info.buf, &client_info.buffer_pos);
				send_code(client_info.fd, 500);
			}
		}
	}

	if (client_info.fd != 0)
	{
		close(client_info.fd);
		client_info.fd = 0;
	}

	return NULL;
}

// prints out usage
int help()
{
	printf("adoftp - anonymous download-only FTP server\n");
	printf("option:\n");
	printf("  -p port         starts listening on the specified port (default 21)\n");
	printf("  -h              prints help (this info)\n");
	printf("  -s ip           listens on the specified IP address (default 0.0.0.0)\n");
	printf("  -d dir          uses the specified directory as the base directory\n");

	return 0;
}

// main entry point
int main(int argc, char * argv[])
{
	char * source_addr = "0.0.0.0";
	int source_port = 21;
	strcpy(basedir, "/");

	int c;
	while ((c = getopt (argc, argv, ":s:p:d:h")) != -1)
	{
		if (c == 's')
		{
			source_addr = optarg;
		}
		else if (c == 'p')
		{
			source_port = atoi(optarg);
		}
		else if (c == 'd')
		{
			strcpy(basedir, optarg);
		}
		else if (c == ':')
		{
			printf("-%c requires an argument\n", optopt);
			return 1;
		}
		else if (c == 'h')
		{
			return help();
		}
		else if (c == '?')
		{
			printf("Unrecognized parameter. Use '-h' for help.\n");
			return 1;
		}
	}

	struct stat s;
	if (stat(basedir, &s) == -1)
	{
		printf("The specified base directory does not exist.\n");
		return 1;
	}

	char tmpbasedir[PATH_MAX + 1] = { 0 };
	if (! realpath(basedir, tmpbasedir)) epicfail("realpath");
	strcpy(basedir, tmpbasedir);

	printf("adoftp server is starting...\n");
	printf("base directory is %s\n", basedir);

	if (strcmp(basedir, "/") == 0) strcpy(basedir, "");

	printf("listening on %s:%d\n", source_addr, source_port);
	int server = create_tcp_server_socket(source_addr, source_port);

	while (1)
	{
		int client = accept_connection(server);
		pthread_t thread_id;
		int result = pthread_create(&thread_id, NULL, thread_proc, (void *)client);
		if (result) epicfail("pthread_create");
	}
}
