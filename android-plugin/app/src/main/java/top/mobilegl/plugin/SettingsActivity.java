package top.mobilegl.plugin;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * In-APK renderer settings. The FCL launcher's own renderer settings cannot be
 * relied on for user-toggled env entries (values may never reach the game process),
 * so this screen writes a key=value config file that libMobileGL.so reads at init
 * from inside the game process (see ConfigLoader's InitializeAcceptedConfigFileVariables).
 *
 * Primary file location is /sdcard/MobileGL/mobilegl.conf - SHARED storage both the
 * plugin (write) and the launcher's game process (read) can access. Writing there
 * needs this app to hold MANAGE_EXTERNAL_STORAGE (all-files access); the screen
 * offers a one-tap grant. A legacy copy is kept in this app's own external files dir
 * for launchers that can read it. NOTE: on Android 11+ no app can read another
 * app's Android/data subtree even with all-files access, so the shared location is
 * what actually matters.
 *
 * File keys written here override same-named process env entries; deleting the file
 * restores launcher-env/default behaviour. A change takes effect on the next game
 * process start: the backend type and the ANGLE choice are latched at libMobileGL
 * init, so Minecraft must be fully killed (swipe away) after saving.
 */
public final class SettingsActivity extends Activity {
    private static final String CONFIG_FILE_NAME = "mobilegl.conf";
    private static final File SHARED_CONFIG_DIR = new File("/sdcard/MobileGL");
    private static final File SHARED_CONFIG_FILE = new File(SHARED_CONFIG_DIR, CONFIG_FILE_NAME);

    private static final String KEY_BACKEND_TYPE = "MOBILEGL_BACKEND_TYPE";
    private static final String KEY_USE_ANGLE = "MOBILEGL_ESPRYT_USE_ANGLE";
    private static final String KEY_MAGMA_DISABLE_SUBGROUP = "MOBILEGL_MAGMA_DISABLE_SUBGROUP";
    private static final String KEY_MAGMA_R11G11B10F_FALLBACK = "MOBILEGL_MAGMA_R11G11B10F_FALLBACK";

    private static final int COLOR_BACKGROUND = 0xFF121212;
    private static final int COLOR_ROW_EVEN = 0xFF1A1A1A;
    private static final int COLOR_TEXT = 0xFFEEEEEE;
    private static final int COLOR_INFO = 0xFF9E9E9E;
    private static final int COLOR_DETAIL = 0xFFBBBBBB;
    private static final int COLOR_PASS = 0xFF4CAF50;
    private static final int COLOR_ERROR = 0xFFF44336;

    private RadioGroup backendGroup;
    private CheckBox vulkanCompatCheck;
    private RadioGroup angleGroup;
    private TextView statusView;
    private Button permissionButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout contentLayout = new LinearLayout(this);
        contentLayout.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(16);
        contentLayout.setPadding(padding, padding, padding, padding);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(COLOR_BACKGROUND);
        scrollView.setFillViewport(true);
        scrollView.addView(contentLayout);
        setContentView(scrollView);

        addText(contentLayout, "MobileGL 渲染设置", 20, COLOR_TEXT, true, 0);
        addText(contentLayout,
                "保存后需完全杀掉 Minecraft 进程（上滑清后台）再重新进入世界才会生效。",
                12, COLOR_INFO, false, dp(6));

        // ---- Storage permission (needed to write /sdcard/MobileGL/) ----
        permissionButton = new Button(this);
        permissionButton.setAllCaps(false);
        permissionButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        permissionButton.setOnClickListener(v -> requestAllFilesAccess());
        LinearLayout.LayoutParams permParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        permParams.topMargin = dp(6);
        contentLayout.addView(permissionButton, permParams);

        // ---- Rendering backend ----
        addSectionTitle(contentLayout, "渲染后端");
        backendGroup = new RadioGroup(this);
        backendGroup.addView(makeRadio("DirectGLES（原生 GLES，推荐，兼容性最好）", 0));
        backendGroup.addView(makeRadio("DirectVulkan（Magma，实验性；Mali r38p1 需勾选下方稳定性开关）", 1));
        contentLayout.addView(backendGroup);

        vulkanCompatCheck = new CheckBox(this);
        vulkanCompatCheck.setText("Mali r38p1 Vulkan 稳定性开关（禁用 shader subgroup + R11G11B10F fallback）");
        vulkanCompatCheck.setTextColor(COLOR_DETAIL);
        vulkanCompatCheck.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        vulkanCompatCheck.setChecked(true);
        LinearLayout.LayoutParams checkParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        checkParams.topMargin = dp(2);
        contentLayout.addView(vulkanCompatCheck, checkParams);

        // ---- ANGLE ----
        addSectionTitle(contentLayout, "ANGLE GLES→Vulkan 转译层（仅 DirectGLES 后端生效）");
        angleGroup = new RadioGroup(this);
        angleGroup.addView(makeRadio("开启（默认；经 Vulkan 驱动渲染）", 0));
        angleGroup.addView(makeRadio("关闭（直连原生 Mali GLES 驱动）", 1));
        contentLayout.addView(angleGroup);

        // ---- Actions ----
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams actionsParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        actionsParams.topMargin = dp(16);

        Button saveButton = new Button(this);
        saveButton.setText("保存");
        saveButton.setAllCaps(false);
        saveButton.setOnClickListener(v -> saveSettings());
        actions.addView(saveButton, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        Button resetButton = new Button(this);
        resetButton.setText("恢复默认");
        resetButton.setAllCaps(false);
        resetButton.setOnClickListener(v -> resetSettings());
        LinearLayout.LayoutParams resetParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        resetParams.leftMargin = dp(8);
        actions.addView(resetButton, resetParams);

        contentLayout.addView(actions, actionsParams);

        statusView = addText(contentLayout, "", 11, COLOR_INFO, false, dp(10));

        loadExistingSettings();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshPermissionUi();
    }

    private boolean hasAllFilesAccess() {
        return android.os.Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private void refreshPermissionUi() {
        if (hasAllFilesAccess()) {
            permissionButton.setText("✓ 文件访问权限已授予");
            permissionButton.setEnabled(false);
        } else {
            permissionButton.setText("授予文件访问权限（保存配置需要）");
            permissionButton.setEnabled(true);
        }
    }

    private void requestAllFilesAccess() {
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception fallback) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception ignored) {
                Toast.makeText(this, "无法打开权限设置页", Toast.LENGTH_SHORT).show();
            }
        }
    }

    /** Preselects radios from the current config file; shows its path (or its absence). */
    private void loadExistingSettings() {
        File configFile = SHARED_CONFIG_FILE.isFile()
                ? SHARED_CONFIG_FILE : legacyConfigFile();
        String backendValue = null;
        String angleValue = null;
        boolean vulkanCompat = true;
        if (configFile.isFile()) {
            try (FileInputStream input = new FileInputStream(configFile)) {
                byte[] bytes = new byte[(int) configFile.length()];
                int read = input.read(bytes);
                String content = new String(bytes, 0, Math.max(read, 0), StandardCharsets.UTF_8);
                for (String line : content.split("\n")) {
                    int eq = line.indexOf('=');
                    if (eq < 0) {
                        continue;
                    }
                    String key = line.substring(0, eq).trim();
                    String value = line.substring(eq + 1).trim();
                    if (KEY_BACKEND_TYPE.equals(key)) {
                        backendValue = value;
                    } else if (KEY_USE_ANGLE.equals(key)) {
                        angleValue = value;
                    } else if (KEY_MAGMA_DISABLE_SUBGROUP.equals(key)
                            || KEY_MAGMA_R11G11B10F_FALLBACK.equals(key)) {
                        vulkanCompat &= "1".equals(value);
                    }
                }
            } catch (IOException ignored) {
            }
        }
        if ("DirectVulkan".equals(backendValue)) {
            backendGroup.check(1);
            vulkanCompatCheck.setChecked(vulkanCompat);
        } else {
            backendGroup.check(0);
        }
        if ("0".equals(angleValue)) {
            angleGroup.check(1);
        } else {
            angleGroup.check(0);
        }
        statusView.setTextColor(COLOR_INFO);
        statusView.setText(configFile.isFile()
                ? "当前配置文件：" + configFile.getAbsolutePath()
                : "未创建配置文件（使用默认值）。\n配置文件路径：" + SHARED_CONFIG_FILE.getAbsolutePath());
    }

    private File legacyConfigFile() {
        File external = getExternalFilesDir(null);
        return external == null ? new File(CONFIG_FILE_NAME) : new File(external, CONFIG_FILE_NAME);
    }

    private void saveSettings() {
        if (!hasAllFilesAccess()) {
            statusView.setTextColor(COLOR_ERROR);
            statusView.setText("尚未授予文件访问权限，无法写入 /sdcard/MobileGL/。请点击上方按钮授权后重试。");
            return;
        }
        boolean vulkan = backendGroup.getCheckedRadioButtonId() == 1;
        boolean useAngle = angleGroup.getCheckedRadioButtonId() != 1;

        StringBuilder builder = new StringBuilder();
        builder.append("# MobileGL plugin settings (written by SettingsActivity)\n");
        builder.append("# Takes effect on the next game process start.\n");
        builder.append(KEY_BACKEND_TYPE).append('=')
                .append(vulkan ? "DirectVulkan" : "DirectGLES").append('\n');
        builder.append(KEY_USE_ANGLE).append('=').append(useAngle ? "1" : "0").append('\n');
        if (vulkan && vulkanCompatCheck.isChecked()) {
            builder.append(KEY_MAGMA_DISABLE_SUBGROUP).append("=1\n");
            builder.append(KEY_MAGMA_R11G11B10F_FALLBACK).append("=1\n");
        }
        byte[] bytes = builder.toString().getBytes(StandardCharsets.UTF_8);

        try {
            if (!SHARED_CONFIG_DIR.isDirectory() && !SHARED_CONFIG_DIR.mkdirs()) {
                throw new IOException("无法创建目录 " + SHARED_CONFIG_DIR);
            }
            try (FileOutputStream output = new FileOutputStream(SHARED_CONFIG_FILE)) {
                output.write(bytes);
            }
            // Best-effort legacy copy for launchers reading the plugin-private dir.
            try (FileOutputStream output = new FileOutputStream(legacyConfigFile())) {
                output.write(bytes);
            }
            statusView.setTextColor(COLOR_PASS);
            statusView.setText("已保存：" + SHARED_CONFIG_FILE.getAbsolutePath()
                    + "\n请完全杀掉 Minecraft 后重新进入。");
            Toast.makeText(this, "已保存，重启游戏后生效", Toast.LENGTH_SHORT).show();
        } catch (IOException error) {
            statusView.setTextColor(COLOR_ERROR);
            statusView.setText("保存失败：" + error);
        }
    }

    private void resetSettings() {
        boolean deleted = true;
        if (SHARED_CONFIG_FILE.isFile()) {
            deleted = SHARED_CONFIG_FILE.delete();
        }
        File legacy = legacyConfigFile();
        if (deleted && legacy.isFile()) {
            deleted = legacy.delete();
        }
        if (!deleted) {
            Toast.makeText(this, "删除配置文件失败", Toast.LENGTH_SHORT).show();
            return;
        }
        backendGroup.check(0);
        angleGroup.check(0);
        vulkanCompatCheck.setChecked(true);
        statusView.setTextColor(COLOR_INFO);
        statusView.setText("已恢复默认（配置文件已删除）。\n请完全杀掉 Minecraft 后重新进入。");
        Toast.makeText(this, "已恢复默认", Toast.LENGTH_SHORT).show();
    }

    private TextView makeRadio(String text, int id) {
        android.widget.RadioButton button = new android.widget.RadioButton(this);
        button.setId(id);
        button.setText(text);
        button.setTextColor(COLOR_TEXT);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        return button;
    }

    private void addSectionTitle(LinearLayout parent, String title) {
        TextView view = new TextView(this);
        view.setText(title);
        view.setTextColor(COLOR_TEXT);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setBackgroundColor(COLOR_ROW_EVEN);
        view.setPadding(dp(8), dp(6), dp(8), dp(6));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(16);
        parent.addView(view, params);
    }

    private TextView addText(LinearLayout parent, String text, int sizeSp, int color,
                             boolean bold, int topMarginPx) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = topMarginPx;
        parent.addView(view, params);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
