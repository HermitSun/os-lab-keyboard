
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

			// 处在输入模式并且超过20s则清屏（输出\b来清屏……）
			// 时间似乎是错乱的
			// @See [[kernal/clock.c]]
			int current_time = get_ticks();
			if (current_mode == 0 &&
				((current_time - time_counter) * 1000 / HZ) > 80 * 1000)
			{
				int i;
				for (i = 0; i < 80 * 25 * 2; ++i)
				{
					out_char(p_tty->p_console, '\b');
				}
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
			// 相当于广义的退格
			do_backspace(p_tty);
		}
		else
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
	}
	else
	{
		int raw_code = key & MASK_RAW;
		switch (raw_code)
		{
		case ENTER:
			put_key(p_tty, '\n');
			// 下一行
			++current_line;
			// 特殊字符\n加入缓存
			buf[p_buf] = '\n';
			++p_buf;
			break;
		case BACKSPACE:
			put_key(p_tty, '\b');
			// 退格对当前行和缓存的处理移到后面
			break;
		// 处理TAB
		case TAB:
			put_key(p_tty, '\t');
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
			break;
		// 处理ESC
		case ESC:
			// 如果现在是输入模式，进入搜索模式
			// 如果现在是搜索模式，返回输入模式
			current_mode = current_mode == 0 ? 1 : 0;
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

		// TAB需要输出4个空格
		if (ch == '\t')
		{
			int i;
			for (i = 0; i < 4; ++i)
			{
				out_char(p_tty->p_console, ' ');
			}
		}
		else if (ch == '\b')
		{
			do_backspace(p_tty);
		}
		// 其他字符直接输出
		else
		{
			out_char(p_tty->p_console, ch);
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
		out_char(p_tty->p_console, *p++);
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

// 处理退格
PRIVATE void do_backspace(TTY *p_tty)
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
			out_char(p_tty->p_console, '\b');
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
			out_char(p_tty->p_console, '\b');
		}
	}
	// 其他情况退一格
	else
	{
		out_char(p_tty->p_console, '\b');
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