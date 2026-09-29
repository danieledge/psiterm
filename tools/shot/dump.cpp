// Feeds a byte stream through libvterm exactly as PsiTerm does, then dumps each
// cell with PsiTerm's colour rules and glyph mapping for the screenshot renderer.
#include <cstdio>
#include <cstdlib>
extern "C" {
#include "vterm.h"
}
typedef unsigned int TUint; typedef int TInt;
TInt PsiMapToCodePage(TUint aCode); TInt PsiBoxSegments(TUint aCode);
static int GreyOf(VTermScreen* s, VTermColor c){ vterm_screen_convert_color_to_rgb(s,&c); int lum=(c.rgb.red*30+c.rgb.green*59+c.rgb.blue*11)/100; return lum/17; }
int main(int argc,char**argv){
  int R=atoi(argv[2]),C=atoi(argv[3]);
  VTerm* vt=vterm_new(R,C); vterm_set_utf8(vt,1); VTermScreen* s=vterm_obtain_screen(vt);
  vterm_screen_enable_altscreen(s,1);
  VTermColor fg,bg; vterm_color_rgb(&fg,0,0,0); vterm_color_rgb(&bg,255,255,255);
  vterm_state_set_default_colors(vterm_obtain_state(vt),&fg,&bg); vterm_screen_reset(s,1);
  FILE* f=fopen(argv[1],"rb"); static char buf[1<<22]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  vterm_input_write(vt,buf,n); vterm_screen_flush_damage(s);
  VTermPos cur; vterm_state_get_cursorpos(vterm_obtain_state(vt),&cur);
  printf("%d %d %d %d\n",R,C,cur.row,cur.col);
  for(int r=0;r<R;r++) for(int c=0;c<C;c++){
    VTermPos p={r,c}; VTermScreenCell cell; vterm_screen_get_cell(s,p,&cell);
    int f0=0,b0=15; bool fd=VTERM_COLOR_IS_DEFAULT_FG(&cell.fg), bd=VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
    if(!fd){ f0=GreyOf(s,cell.fg); if(bd) f0=(f0>9)?9-(f0-9)/2:f0*2/3; }
    if(!bd) b0=GreyOf(s,cell.bg);
    if(cell.attrs.reverse){int t=f0;f0=b0;b0=t;}
    int d=f0-b0; if(d<0)d=-d; if(d<6) f0=(b0>=8)?0:15;
    unsigned ch=cell.chars[0]; if(ch==(unsigned)-1) ch=0;
    int seg=ch?PsiBoxSegments(ch):0; int byte=ch?PsiMapToCodePage(ch):32;
    printf("%u %d %d %d %d %d %d %d\n",ch,f0,b0,cell.attrs.bold,cell.attrs.underline,seg,byte,cell.width);
  }
}
