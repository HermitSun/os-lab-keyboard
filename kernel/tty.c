
/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                               tty.c
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                                                    Forrest Yu, 2005
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/

#include "type.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "proc.h"
#include "tty.h"
#include "console.h"
#include "global.h"
#include "keyboard.h"
#include "proto.h"

#define TTY_FIRST (tty_table)
#define TTY_END (tty_table + NR_CONSOLES)

PRIVATE void init_tty(TTY *p_tty);
PRIVATE void tty_do_read(TTY *p_tty);
PRIVATE void tty_do_write(TTY *p_tty);
PRIVATE void put_key(TTY *p_tty, u32 key);

// 清空屏幕
PRIVATE void clear_screen(TTY *p_tty);
// 退格方法
PRIVATE void do_backspace(TTY *p_tty);

// 输入模式
// int MODE_INPUT = 0;
// 搜索模式
// int MODE_SEARCH = 1;

// 当前状态
int current_mode;
// 缓存输入的字符，用于搜索
char buf[80 * 25];
int p_buf;
// 每一行长度
int line_length[25 * 2];
int current_line;
// 搜索模式的输入
char search_buf[80 * 25];
int p_search_buf;
// 搜索是否已完成
int indexs[80 * 25];
int search_has_done;

/*======================================================================*
                           task_tty
 *======================================================================*/
PUBLIC void task_tty()
{
	TTY *p_tty;

	init_keyboard();

	for (p_tty = TTY_FIRST; p_tty < TTY_END; p_tty++)
	{
		init_tty(p_tty);
	}
	select_console(0);

	// 初始为输入模式
	current_mode = 0;
	// 开始计时
	// -20 * 1000是为了先清屏一次
	int time_counter = get_ticks() - 80 * 1000;
	// 初始化缓存区指针
	p_buf = 0;
	int i;
	for (i = 0; i < 80 * 25; ++i)
	{
		buf[i] = 0;
	}
	// 初始化每一行的长度
	for (i = 0; i < 25 * 2; ++i)
	{
		line_length[i] = 0;
	}
	current_line = 0;

	while (1)
	{
		for (p_tty = TTY_FIRST; p_tty < TTY_END; p_tty++)
		{
			tty_do_read(p_tty);
			tty_do_write(p_tty);

			// 处在输入模式并且超过20s则清屏
			// 时间似乎是错乱的，所以凑合一下，选一个比较稳定的数
			// @See [[kernal/clock.c]]
			int current_time = get_ticks();
			if (current_mode == 0 &&
				((current_time - time_counter) * 1000 / HZ) > 80 * 1000)
			{
				clear_screen(p_tty);
				// 重置缓存和每一行长度，否则会导致退格异常
				p_buf = 0;
				for (i = 0; i < 80 * 25; ++i)
				{
					buf[i] = 0;
				}
				for (i = 0; i < 25 * 2; ++i)
				{
					line_length[i] = 0;
				}
				// 重置计时器
				// 但是可以预见，这种方式的误差会越来越大，因为调用需要时间
				time_counter = current_time;
			}
		}
	}
}

/*======================================================================*
			   init_tty
 *======================================================================*/
PRIVATE void init_tty(TTY *p_tty)
{
	p_tty->inbuf_count = 0;
	p_tty->p_inbuf_head = p_tty->p_inbuf_tail = p_tty->in_buf;

	init_screen(p_tty);
}

/*======================================================================*
				in_process
 *======================================================================*/
PUBLIC void in_process(TTY *p_tty, u32 key)
{
	char output[2] = {'\0', '\0'};

	if (!(key & FLAG_EXT))
	{
		// 撤销
		if ((key & MASK_RAW) == 'z' &&
			((key & FLAG_CTRL_L) || (key & FLAG_CTRL_R)))
		{
			// 撤销相当于广义的退格
			do_backspace(p_tty);
		}
		else
		{
			// 只在输入模式下响应
			if (current_mode == 0)
			{
				put_key(p_tty, key);
				// 可输出字符加入缓存
				// 假设不会溢出
				buf[p_buf] = key;
				++p_buf;
				// 普通字符长度 + 1
				if (line_length[current_line] + 1 > 80)
				{
					int temp = line_length[current_line];
					line_length[current_line] = 80;
					++current_line;
					line_length[current_line] = temp + 1 - 80;
				}
				else
				{
					++line_length[current_line];
				}
			}
			// 搜索模式的输入是另外一种输入（会被自动清空的输入）
			else
			{
				put_key(p_tty, key);
				// 可输出字符加入搜索缓存
				// 假设不会溢出
				search_buf[p_search_buf] = key;
				++p_search_buf;
			}
		}
	}
	else
	{
		int raw_code = key & MASK_RAW;
		switch (raw_code)
		{
		case ENTER:
			// 输入模式的ENTER是换行
			if (current_mode == 0)
			{
				put_key(p_tty, '\n');
				// 下一行
				++current_line;
				// 特殊字符\n加入缓存
				buf[p_buf] = '\n';
				++p_buf;
			}
			// 搜索模式的ENTER是确认
			// 所以这也默认了搜索模式不会出现换行（
			else
			{
				int input_length = strlen(buf);
				int search_length = strlen(search_buf);
				// 初始化找到的index
				int i;
				for (i = 0; i < 80 * 25; ++i)
				{
					indexs[i] = 0;
				}
				// 进行搜索
				for (i = 0; i < input_length - search_length + 1; ++i)
				{
					int is_equal = 1;
					int j;
					for (j = 0; j < search_length; ++j)
					{
						if (buf[i + j] != search_buf[j])
						{
							is_equal = 0;
							break;
						}
					}
					if (is_equal)
					{
						// 考虑搜索到的内容重合，使用这种方式可以重复标记
						for (j = 0; j < search_length; ++j)
						{
							indexs[i + j] = 1;
						}
					}
				}
				// 搜索完成，交给输出函数进行处理
				search_has_done = 1;
				clear_screen(p_tty);
				put_key(p_tty, '\n');
			}
			break;
		// 两种模式都支持退格
		case BACKSPACE:
			put_key(p_tty, '\b');
			// 退格对当前行和缓存的处理移到后面
			break;
		// 处理TAB
		case TAB:
			put_key(p_tty, '\t');
			// 输入模式的TAB
			if (current_mode == 0)
			{
				// TAB换行
				if (line_length[current_line] + 4 > 80)
				{
					int temp = line_length[current_line];
					line_length[current_line] = 80;
					++current_line;
					line_length[current_line] = temp + 4 - 80;
				}
				else
				{
					line_length[current_line] += 4;
				}
				// 特殊字符\t加入缓存
				buf[p_buf] = '\t';
				++p_buf;
			}
			// 搜索模式的TAB
			else
			{
				// 加入搜索缓存
				search_buf[p_search_buf] = '\t';
				++p_search_buf;
			}
			break;
		// 处理ESC
		case ESC:
			// 如果现在是输入模式，进入搜索模式
			// 如果现在是搜索模式，返回输入模式
			current_mode = current_mode == 0 ? 1 : 0;
			// 切换模式时初始化搜索输入
			int i;
			for (i = 0; i < 80 * 25; ++i)
			{
				search_buf[i] = 0;
				indexs[i] = 0;
			}
			p_search_buf = 0;
			// 重置搜索状态
			search_has_done = 0;
			// 切换后回到输入模式，先清空屏幕
			if (current_mode == 0)
			{
				clear_screen(p_tty);
				put_key(p_tty, 0x1B);
			}
			break;
		case UP:
			if ((key & FLAG_SHIFT_L) || (key & FLAG_SHIFT_R))
			{
				scroll_screen(p_tty->p_console, SCR_DN);
			}
			break;
		case DOWN:
			if ((key & FLAG_SHIFT_L) || (key & FLAG_SHIFT_R))
			{
				scroll_screen(p_tty->p_console, SCR_UP);
			}
			break;
		case F1:
		case F2:
		case F3:
		case F4:
		case F5:
		case F6:
		case F7:
		case F8:
		case F9:
		case F10:
		case F11:
		case F12:
			/* Alt + F1~F12 */
			if ((key & FLAG_ALT_L) || (key & FLAG_ALT_R))
			{
				select_console(raw_code - F1);
			}
			break;
		default:
			break;
		}
	}
}

/*======================================================================*
			      put_key
*======================================================================*/
PRIVATE void put_key(TTY *p_tty, u32 key)
{
	if (p_tty->inbuf_count < TTY_IN_BYTES)
	{
		*(p_tty->p_inbuf_head) = key;
		p_tty->p_inbuf_head++;
		if (p_tty->p_inbuf_head == p_tty->in_buf + TTY_IN_BYTES)
		{
			p_tty->p_inbuf_head = p_tty->in_buf;
		}
		p_tty->inbuf_count++;
	}
}

/*======================================================================*
			      tty_do_read
 *======================================================================*/
PRIVATE void tty_do_read(TTY *p_tty)
{
	if (is_current_console(p_tty->p_console))
	{
		keyboard_read(p_tty);
	}
}

/*======================================================================*
			      tty_do_write
 *======================================================================*/
PRIVATE void tty_do_write(TTY *p_tty)
{
	if (p_tty->inbuf_count)
	{
		char ch = *(p_tty->p_inbuf_tail);
		p_tty->p_inbuf_tail++;
		if (p_tty->p_inbuf_tail == p_tty->in_buf + TTY_IN_BYTES)
		{
			p_tty->p_inbuf_tail = p_tty->in_buf;
		}
		p_tty->inbuf_count--;

		// 搜索模式
		if (search_has_done == 1)
		{
			int input_length = strlen(buf);
			int search_length = strlen(search_buf);
			int i;
			for (i = 0; i < input_length; ++i)
			{
				// 如果是搜索结果
				if (indexs[i] == 1)
				{
					// TAB和空格用白底来体现
					if (buf[i] == '\t')
					{
						out_char(p_tty->p_console, ' ', 2);
						out_char(p_tty->p_console, ' ', 2);
						out_char(p_tty->p_console, ' ', 2);
						out_char(p_tty->p_console, ' ', 2);
					}
					else if (buf[i] == ' ')
					{
						out_char(p_tty->p_console, ' ', 2);
					}
					// 其他的是红字
					else
					{
						out_char(p_tty->p_console, buf[i], 1);
					}
				}
				// 不是搜索结果就正常输出
				else
				{
					// TAB还是4个空格
					if (buf[i] == '\t')
					{
						out_char(p_tty->p_console, ' ', 0);
						out_char(p_tty->p_console, ' ', 0);
						out_char(p_tty->p_console, ' ', 0);
						out_char(p_tty->p_console, ' ', 0);
					}
					else
					{
						out_char(p_tty->p_console, buf[i], 0);
					}
				}
			}
			// 同时还要输出搜索内容本身
			for (i = 0; i < search_length; ++i)
			{
				// TAB和空格用白底来体现
				if (search_buf[i] == '\t')
				{
					out_char(p_tty->p_console, ' ', 2);
					out_char(p_tty->p_console, ' ', 2);
					out_char(p_tty->p_console, ' ', 2);
					out_char(p_tty->p_console, ' ', 2);
				}
				else if (search_buf[i] == ' ')
				{
					out_char(p_tty->p_console, ' ', 2);
				}
				// 其他的是红字
				else
				{
					out_char(p_tty->p_console, search_buf[i], 1);
				}
			}
			// 关闭搜索模式
			search_has_done = 0;
		}
		// 搜索模式向非搜索模式切换
		else if (ch == 0x1B)
		{
			int input_length = strlen(buf);
			int i;
			for (i = 0; i < input_length; ++i)
			{
				// TAB还是4个空格
				if (buf[i] == '\t')
				{
					out_char(p_tty->p_console, ' ', 0);
					out_char(p_tty->p_console, ' ', 0);
					out_char(p_tty->p_console, ' ', 0);
					out_char(p_tty->p_console, ' ', 0);
				}
				else
				{
					out_char(p_tty->p_console, buf[i], 0);
				}
			}
		}
		// 非搜索模式
		else
		{
			// TAB需要输出4个空格
			if (ch == '\t')
			{
				int i;
				for (i = 0; i < 4; ++i)
				{
					out_char(p_tty->p_console, ' ', 0);
				}
			}
			else if (ch == '\b')
			{
				do_backspace(p_tty);
			}
			// 其他字符直接输出
			else
			{
				out_char(p_tty->p_console, ch, 0);
			}
		}
	}
}

/*======================================================================*
                              tty_write
*======================================================================*/
PUBLIC void tty_write(TTY *p_tty, char *buf, int len)
{
	char *p = buf;
	int i = len;

	while (i)
	{
		out_char(p_tty->p_console, *p++, 0);
		i--;
	}
}

/*======================================================================*
                              sys_write
*======================================================================*/
PUBLIC int sys_write(char *buf, int len, PROCESS *p_proc)
{
	tty_write(&tty_table[p_proc->nr_tty], buf, len);
	return 0;
}

// 输出\b来清屏……
PRIVATE void clear_screen(TTY *p_tty)
{
	int i;
	for (i = 0; i < 80 * 25 * 2; ++i)
	{
		out_char(p_tty->p_console, '\b', 0);
	}
}

// 处理退格
PRIVATE void do_backspace(TTY *p_tty)
{
	// 输入模式的退格
	if (current_mode == 0)
	{
		// 回到缓存真正的头部，同时要避免越界
		// 无论如何，当前字符都要退出缓存
		if (p_buf > 0)
		{
			--p_buf;
		}
		// TAB需要退4格
		// 不知道为啥TAB变成0x9了
		if (buf[p_buf] == 0x9)
		{
			int i;
			for (i = 0; i < 4; ++i)
			{
				out_char(p_tty->p_console, '\b', 0);
			}

			// TAB换行
			if (line_length[current_line] - 4 < 0)
			{
				int temp = line_length[current_line];
				line_length[current_line] = 0;
				// 防止越界
				if (current_line - 1 >= 0)
				{
					--current_line;
					if (line_length[current_line] - (4 - temp) >= 0)
					{
						line_length[current_line] = line_length[current_line] - (4 - temp);
					}
				}
			}
			else
			{
				line_length[current_line] -= 4;
			}
		}
		// ENTER需要退一行
		// 不知道为啥ENTER变成0xA了
		else if (buf[p_buf] == 0xA)
		{
			int i;
			line_length[current_line] = 0;
			// 防止越界
			if (current_line - 1 >= 0)
			{
				--current_line;
			}
			for (i = 0; i < 80 - line_length[current_line]; ++i)
			{
				out_char(p_tty->p_console, '\b', 0);
			}
		}
		// 其他情况退一格
		else
		{
			out_char(p_tty->p_console, '\b', 0);
			// 普通字符 - 1
			if (line_length[current_line] - 1 < 0)
			{
				line_length[current_line] = 0;
				// 防止越界
				if (current_line - 1 >= 0)
				{
					--current_line;
					if (line_length[current_line] - 1 >= 0)
					{
						--line_length[current_line];
					}
				}
			}
			else
			{
				--line_length[current_line];
			}
		}
		// 清除当前字符的缓存
		buf[p_buf] = 0;
	}
	// 搜索模式的退格
	else
	{
		// 回到缓存真正的头部，同时要避免越界
		// 无论如何，当前字符都要退出缓存
		if (p_search_buf > 0)
		{
			--p_search_buf;
		}
		// 退到底不再处理，否则会影响之前输入的内容
		else
		{
			return;
		}
		// TAB需要退4格
		// 不知道为啥TAB变成0x9了
		if (search_buf[p_search_buf] == 0x9)
		{
			int i;
			for (i = 0; i < 4; ++i)
			{
				out_char(p_tty->p_console, '\b', 0);
			}
		}
		// 其他情况退一格
		else
		{
			out_char(p_tty->p_console, '\b', 0);
		}
		// 清除当前字符的缓存
		search_buf[p_search_buf] = 0;
	}
}

// 字符串比较函数
PUBLIC int strcmp(const char *src, const char *dst)
{
	int ret = 0;
	while (!(ret = *(unsigned char *)src - *(unsigned char *)dst) && *dst)
		++src, ++dst;
	if (ret < 0)
		ret = -1;
	else if (ret > 0)
		ret = 1;
	return (ret);
}