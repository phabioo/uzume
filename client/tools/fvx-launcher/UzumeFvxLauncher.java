// Uzume launcher for the randomizer engine Universal Pokemon Randomizer FVX (issue #414).
//
// FVX's RomOpener refuses 3DS games from the command line (EXTRA_MEMORY_NOT_AVAILABLE) unless setExtraMemoryAvailable(true)
// was called; only the GUI calls it, the CLI (com.uprfvx.random.cli.CliRandomizer) never does. This class sets the flag by
// reflection and then runs the unchanged FVX entry point. It has no compile-time dependency on FVX. It can go once FVX's CLI sets
// the flag itself.
//
// Use: java -cp UPR-FVX.jar<sep>uzume-fvx-launcher.jar UzumeFvxLauncher cli -i ... (same arguments as for -jar UPR-FVX.jar).
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class UzumeFvxLauncher {
  private UzumeFvxLauncher() {}

  static void enableExtraMemory() {
    try {
      Class<?> cli = Class.forName("com.uprfvx.random.cli.CliRandomizer");
      Field f = cli.getDeclaredField("romOpener");
      f.setAccessible(true);
      Object opener = f.get(null);
      Method m = opener.getClass().getMethod("setExtraMemoryAvailable", boolean.class);
      m.invoke(opener, true);
    } catch (Throwable t) {
      System.out.println("WARNING: uzume launcher could not enable 3DS support: " + t.getClass().getName());
    }
  }

  public static void main(String[] args) throws Throwable {
    enableExtraMemory();
    Class<?> gui = Class.forName("com.uprfvx.random.gui.RandomizerGUI");
    Method main = gui.getMethod("main", String[].class);
    try {
      main.invoke(null, (Object) args);
    } catch (InvocationTargetException e) {
      throw e.getCause() != null ? e.getCause() : e;
    }
  }
}
