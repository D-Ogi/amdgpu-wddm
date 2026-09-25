using System;
using System.IO;
using System.Linq;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Windows.Forms;
using System.Web.Script.Serialization;
using System.Runtime.InteropServices;

public sealed class CaseResult {
    public string name, gpu_hash, cpu_hash;
    public bool pass;
    public int mismatches;
    public double dispatch_us;
}
public sealed class Results {
    public int width, height, iterations;
    public CaseResult[] cases;
}
public sealed class Gallery : Form {
    readonly string root;
    readonly Results results;
    readonly Bitmap[] gpu, cpu, diff;
    readonly Font title = new Font("Segoe UI", 28, FontStyle.Bold);
    readonly Font large = new Font("Segoe UI", 20, FontStyle.Bold);
    readonly Font body = new Font("Segoe UI", 12);
    readonly Font small = new Font("Consolas", 10);
    readonly Color ink = Color.FromArgb(231,239,249), muted = Color.FromArgb(147,164,184);
    readonly Timer timer = new Timer();
    int selected;
    bool paused;
    string progress = "Vulkan correctness tests; formal certification pending.";
    static Color Palette(uint value) {
        if(value >= 96) return Color.FromArgb(5,10,22);
        double t = Math.Pow(value / 96.0, 0.50);
        Color[] colors={Color.FromArgb(14,24,65),Color.FromArgb(22,92,156),
            Color.FromArgb(16,206,188),Color.FromArgb(238,230,162),Color.FromArgb(255,99,89)};
        double s=t*4;int a=Math.Min(3,(int)s);double f=s-a;
        return Color.FromArgb((int)(colors[a].R*(1-f)+colors[a+1].R*f),
            (int)(colors[a].G*(1-f)+colors[a+1].G*f),(int)(colors[a].B*(1-f)+colors[a+1].B*f));
    }
    Bitmap Make(uint[] values,uint[] reference,bool difference) {
        Bitmap b=new Bitmap(results.width,results.height,PixelFormat.Format32bppArgb);
        int[] pixels=new int[values.Length];
        for(int i=0;i<pixels.Length;i++)pixels[i]=(difference?
            (values[i]==reference[i]?Color.FromArgb(11,30,38):Color.FromArgb(255,73,106)):Palette(values[i])).ToArgb();
        var data=b.LockBits(new Rectangle(0,0,b.Width,b.Height),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);
        try{Marshal.Copy(pixels,0,data.Scan0,pixels.Length);}finally{b.UnlockBits(data);}
        return b;
    }
    uint[] LoadWords(string path) {
        byte[] bytes=File.ReadAllBytes(path);
        if(bytes.Length!=results.width*results.height*4)throw new InvalidDataException("Wrong image byte count");
        uint[] words=new uint[bytes.Length/4];Buffer.BlockCopy(bytes,0,words,0,bytes.Length);return words;
    }
    public Gallery(string directory) {
        root=directory;
        results=new JavaScriptSerializer().Deserialize<Results>(File.ReadAllText(Path.Combine(root,"results.json")));
        if(results.cases==null || results.cases.Length==0)throw new InvalidDataException("No measured results");
        gpu=new Bitmap[results.cases.Length];cpu=new Bitmap[gpu.Length];diff=new Bitmap[gpu.Length];
        for(int i=0;i<gpu.Length;i++) {
            var c=results.cases[i];uint[] g=LoadWords(Path.Combine(root,c.name+"-gpu.bin"));
            uint[] r=LoadWords(Path.Combine(root,c.name+"-cpu.bin"));
            int actual=g.Where((v,j)=>v!=r[j]).Count();
            if(actual!=c.mismatches || c.pass!=(actual==0))throw new InvalidDataException("Comparison metadata disagrees with pixels");
            gpu[i]=Make(g,r,false);cpu[i]=Make(r,r,false);diff[i]=Make(g,r,true);
        }
        Text="BC-250 | Shader Observatory | measured GPU output";
        BackColor=Color.FromArgb(7,12,23);ForeColor=ink;
        DoubleBuffered=true;KeyPreview=true;
        StartPosition=FormStartPosition.Manual;
        var screen=Screen.PrimaryScreen.WorkingArea;
        ClientSize=new Size(Math.Min(1320,screen.Width-100),Math.Min(850,screen.Height-100));
        Location=new Point(20,Math.Max(0,(screen.Height-Height)/2));
        MinimumSize=new Size(960,720);
        timer.Interval=8000;
        timer.Tick+=(s,e)=>{if(!paused)selected=(selected+1)%gpu.Length;ReadProgress();Invalidate();};
        timer.Start();
        KeyDown+=(s,e)=>{
            if(e.KeyCode==Keys.Space){paused=!paused;Invalidate();}
            if(e.KeyCode==Keys.Right){selected=(selected+1)%gpu.Length;Invalidate();}
            if(e.KeyCode==Keys.Left){selected=(selected+gpu.Length-1)%gpu.Length;Invalidate();}
            if(e.KeyCode==Keys.Escape)Close();
        };
        ReadProgress();
    }
    void ReadProgress() {
        try{var p=Path.Combine(root,"progress.txt");if(File.Exists(p))progress=File.ReadAllText(p).Trim();}catch(IOException){}
    }
    void Label(Graphics g,string text,Font font,Color color,float x,float y) {
        using(var brush=new SolidBrush(color))g.DrawString(text,font,brush,x,y);
    }
    void ImagePanel(Graphics g,Bitmap image,Rectangle rect,string heading,string caption) {
        using(var b=new SolidBrush(Color.FromArgb(14,23,38)))g.FillRectangle(b,rect.X-10,rect.Y-48,rect.Width+20,rect.Height+95);
        Label(g,heading,body,ink,rect.X,rect.Y-37);
        g.InterpolationMode=InterpolationMode.NearestNeighbor;g.PixelOffsetMode=PixelOffsetMode.Half;
        g.DrawImage(image,rect);
        Label(g,caption,small,muted,rect.X,rect.Bottom+13);
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);var g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;
        g.TextRenderingHint=System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        int w=ClientSize.Width,h=ClientSize.Height;var c=results.cases[selected];
        Label(g,"BC-250  /  SHADER OBSERVATORY",title,ink,30,24);
        Label(g,"Real Vulkan compute output, independently checked on the CPU",body,muted,33,77);
        Color status=c.pass?Color.FromArgb(28,218,170):Color.FromArgb(255,80,107);
        Label(g,c.pass?"PIXELS MATCH":"MISMATCH DETECTED",large,status,w-310,32);
        Label(g,string.Format("{0:N0} pixels  /  {1:N0} differences",results.width*results.height,c.mismatches),small,muted,w-310,73);
        int side=Math.Min((w-120)/3,h-350),y=200;
        int x1=35,x2=x1+side+30,x3=x2+side+30;
        Label(g,c.name.Replace("-"," "),large,ink,33,123);
        Label(g,string.Format("96 iterations | fixed-point arithmetic | sample {0}/{1}",selected+1,gpu.Length),small,muted,400,135);
        ImagePanel(g,gpu[selected],new Rectangle(x1,y,side,side),"01   GPU / RADV + ACO","Shader results read back from the BC-250");
        ImagePanel(g,cpu[selected],new Rectangle(x2,y,side,side),"02   CPU reference","Independent C implementation");
        ImagePanel(g,diff[selected],new Rectangle(x3,y,side,side),"03   Difference map","Dark = equal   /   pink = mismatch");
        int bottom=y+side+78;
        Label(g,"GPU "+c.gpu_hash+"    CPU "+c.cpu_hash,small,muted,35,bottom);
        Label(g,string.Format("Compute submit + wait: {0:N1} us (one sample, not a benchmark)",c.dispatch_us),small,muted,35,bottom+25);
        Label(g,progress,body,ink,35,Math.Max(bottom+65,h-116));
        Label(g,"Captured result gallery. No continuous GPU workload. Colors encode measured iteration counts.",small,muted,35,h-67);
        Label(g,(paused?"Paused":"Auto cycle")+"  |  Space: pause   Left/Right: select   Esc: close  |  DWM presentation: llvmpipe CPU",small,muted,35,h-42);
    }
    protected override void Dispose(bool disposing) {
        if(disposing){timer.Dispose();foreach(var b in gpu)b.Dispose();foreach(var b in cpu)b.Dispose();foreach(var b in diff)b.Dispose();title.Dispose();large.Dispose();body.Dispose();small.Dispose();}
        base.Dispose(disposing);
    }
    [STAThread] public static void Main(string[] args) {
        if(args.Length!=1)return;
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        try{Application.Run(new Gallery(args[0]));}
        catch(Exception e){File.WriteAllText(Path.Combine(args[0],"viewer-error.txt"),e.ToString());}
    }
}
