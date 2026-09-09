# ChlorineOS 开发日志
此文件请在上传至github时在gitignore中忽略
## 结构图
-Cl_OS
    -boot
        -boot.asm
        -loader.asm
    -build
        -boot.bin
        -kernel.elf
        -kernel.bin
        -loader.bin
        -main.o
    -kernel
        -kernel.c
        -entry.asm
        -fat12.h
        -fat12.c
        -fat32.h
        -fat32.c
        -kernel.bin
        -kernel.elf
        -link.ld
        -process.c
        -process.h
        -program.h
        -syscall.h
    -include
        -a20.inc
        -gdt.inc
        -print.inc
    -programs
        -shell.c
        -test.c
    -tools
        -add_header.c
    -dev_log.md
    -bmca_os.img
    -Makefile
    -linker.ld
    -Makefile
    -hdd.img
## 详情
**2025-8-24**
开工大吉
BMCA_OS主文件夹产生，创建boot.asm
**2025-8-24** 
boot.asm编写完成并成功通过了测试，用QEMU在终端上打印了字符
修改任意字符不受影响
**2025-9-11** 
重复测试并有了二级文件夹boot
编写了makefile进行运行，成功
**2025-9-12**
建立了文件loader.asm，用makerfile测试成功
**2025-9-12**
开发include部分，加入了a20.inc gdt.inc print.inc
运行的还可以
但是保护模式与kernel死活进不去
**2025-9-14**
重新调整了文件
但是还是卡死/进不去
有些时候干脆直接不显示
**2025-10-1**
因特殊原因bmca_os暂停开发，重启时间未知
**2026-6-2**
bmca_os项目重新启动
成功进入保护模式，正式进入kernel部分的开发
成功的显示了kernel的消息，成功跨越分水岭！
建立entry.asm作为中间转接
成功的创建了一个无限字符输入器（集成在kernel.c中）
成功创建了一个Shell，并且加入了type命令进入无限字符输入器
无限字符输入器输入命令exit退出
加入命令help，可以弹出所有指令
加入命令shutdown，可以退出QEMU虚拟机
准备做文件处理
选定FAT12文件处理
加入命令 cat ls
遇到黑屏
解决，发现只是扇区不够，开到16就好
成功解决文件处理外壳
**2026-6-3**
加入gui功能，使得可以有一个色块
加入光标
扇区调整到32
加入系统图标，但没有做光标交互
认为GUI有点难，暂停这部分的开发
继续制作文件处理，主要用了FAT32
加入文件fat32.h fat32.c
在fat32.c中加入查询文件与文件内容部分
添加命令lsdisk 和 catdisk 从FAT32做文件处理
加入命令edit作为文本编辑器，可以有保存于切换行功能，bug：保存失效
bug被解决
删除type功能，认为比较没用
将扇区调整到64，避免加载不出来
加入修改存在于新建文档功能，仅支持txt，目前有bug
加入rm删除功能，可删除txt，目前有bug
扇区被扩大到128
**2026-6-5**
一直在解决bug
修改文件内容遇到bug
修改文件内容bug被修复
删除功能bug被修复
保存出现bug
为了防止功能打架，新建了一个new指令来创造文件
删掉edit的创建文件功能
修复从创建文件bug，但是没修好，创建一个之后会有一个错误文件TESTTXT.TXT生成，无法打开
**2026-6-11**
解决了这个困扰许久的bug，这个代码实际上能跑而且运行的非常好，我之前测试是因为我每次新建文件的时候都是新建一个名叫test.txt的文件，导致会生成一个错误的TESTTXT.TXT文件，无法打开并删除，现在我知道了，我只需要新建一个文件（不需要加后缀）就可以新建，而且功能都可以
加入指令snake，可以做一个小的贪吃蛇游戏
后续想法：可以继续优化GUI/新功能
目前来看这个系统没有任何明显的bug，就是记住不要在文件后面加后缀
**2026-6-22**
加入颜色处理功能color
改变了整体输入 采用 [指令] [参数] 的形式输入
试图做多任务处理，但是失败了
**2026-6-24**
BMCA_OS因未知原因停止开发，重启时间未知
**2026-7-29**
BMCA_OS复工
解决了以上所有bug，包括文件处理/编辑的bug
计划做文件夹的概念
文件夹概念成了，加入新指令mk,cd,改变ls指令
bug命名系统成立
bug1：无论处于什么目录时，新建文件/文件夹只会在根目录新建
BMCA_OS改名为ChlorineOS并存在简称Cl_OS
将此项目在github上添加了一个库
写了README.md
思考如何在不使用TigerVNC的情况下让QEMU自己弹出窗口
**2026-7-30**
想做多任务处理
改了许多代码，增加了注册表命令包括run ps kill
增加了进程/程序的概念，打算不仅依赖于kernel.c
增加文件夹programs tools 
增加文件shell.c test.c add_header.c process.h program.h syscall.h process.c
第一次启动黑屏，从底层开始修复
利用deepseek和gemini的齐心协力终于能运行到kernel层了
发现根本到不了\>
依旧修bug/. 然后看到\>了
结果没办法输入
修了2个小时之后可以输入了，但是无法识别指令
bug2: 无法识别指令
没办法调试了1天也没办法，明天再说
**2026-7-31**
发现是扇区开小了，这问题是真阴魂不散是吧 bug2解决
bug3：ps指令无法正确识别进程
解决bug3 bug4:循环输出进程直到卡爆
**2026-8-1**
您猜我解决了吗，没有，和gemini和deepseek雷霆对峙了几个小时之后我累了
**2026-8-9**
解决bug4 但似乎多任务还是不行



















































