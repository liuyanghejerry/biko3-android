package io.github.hyc1743.biko3;

import android.content.res.AssetManager;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * Installs the game's Data tree from the six .b3data volumes, mirroring the iOS
 * Biko3ResourceInstaller: the shipped manifest names every part and every
 * original file (size, SHA-256, and the segments that carry it). Volumes are
 * verified before anything is written; files are unpacked through a temporary
 * file and renamed only after their own digest matches.
 *
 * Volumes are looked for in the app's external files directory (push them with
 * adb) and in its ResourcePacks subdirectory.
 */
final class ResourceImporter {
    interface Progress { void update(String phase, float fraction); }

    private static final String TAG = "Biko3Import";
    private static final int BUFFER = 1 << 20;

    private final AssetManager assets;
    private final File gameRoot;      // <external>/game
    private final File dataDir;       // <external>/game/Data
    private final List<File> partDirs = new ArrayList<>();

    ResourceImporter(AssetManager assets, File externalRoot) {
        this.assets = assets;
        this.gameRoot = new File(externalRoot, "game");
        this.dataDir = new File(gameRoot, "Data");
        partDirs.add(new File(externalRoot, "ResourcePacks"));
        partDirs.add(gameRoot);
        partDirs.add(externalRoot);
    }

    /** True when the game's first archive is already in place and non-empty. */
    boolean dataReady() {
        File marker = new File(dataDir, "bk3_00.pp");
        return marker.isFile() && marker.length() > 0;
    }

    private JSONObject manifest() throws Exception {
        try (InputStream input = assets.open("guest/Biko3Data.manifest.json")) {
            byte[] raw = readAll(input);
            return new JSONObject(new String(raw, "UTF-8"));
        }
    }

    private static byte[] readAll(InputStream input) throws IOException {
        byte[] buffer = new byte[65536];
        java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
        int read;
        while ((read = input.read(buffer)) > 0) output.write(buffer, 0, read);
        return output.toByteArray();
    }

    /** True when every volume named by the manifest is already in place. */
    boolean volumesAvailable() {
        try {
            JSONArray parts = manifest().getJSONArray("parts");
            for (int i = 0; i < parts.length(); i++) {
                String name = parts.getJSONObject(i).getString("name");
                long size = parts.getJSONObject(i).getLong("size");
                File file = findPart(name);
                if (file == null) {
                    Log.i(TAG, "volume missing: " + name + " in " + partDirs);
                    return false;
                }
                if (file.length() != size) {
                    Log.i(TAG, "volume size mismatch: " + file + " " + file.length() + " != " + size);
                    return false;
                }
            }
            Log.i(TAG, "volumes available");
            return true;
        } catch (Throwable problem) {
            Log.w(TAG, "volume scan failed", problem);
            return false;
        }
    }

    private File findPart(String name) {
        for (File directory : partDirs) {
            File candidate = new File(directory, name);
            if (candidate.isFile()) return candidate;
        }
        return null;
    }

    /** Verify the volumes, then unpack every file; returns null on success or an error message. */
    String install(Progress progress) throws Exception {
        JSONObject manifest = manifest();
        JSONArray parts = manifest.getJSONArray("parts");
        JSONArray files = manifest.getJSONArray("files");

        File[] partFiles = new File[parts.length()];
        for (int i = 0; i < parts.length(); i++) {
            JSONObject part = parts.getJSONObject(i);
            String name = part.getString("name");
            File file = findPart(name);
            if (file == null) return "缺少分卷：" + name;
            if (file.length() != part.getLong("size")) return "分卷大小不符：" + name;
            partFiles[i] = file;
        }
        for (int i = 0; i < parts.length(); i++) {
            JSONObject part = parts.getJSONObject(i);
            progress.update("校验分卷 " + (i + 1) + "/" + parts.length(),
                    (float) i / parts.length() * 0.35f);
            String digest = sha256(partFiles[i]);
            if (!digest.equalsIgnoreCase(part.getString("sha256")))
                return "分卷校验失败：" + part.getString("name");
        }
        Log.i(TAG, "parts verified: " + parts.length());

        if (!dataDir.isDirectory() && !dataDir.mkdirs())
            return "无法创建资源目录：" + dataDir;

        RandomAccessFile[] handles = new RandomAccessFile[partFiles.length];
        try {
            for (int i = 0; i < partFiles.length; i++) handles[i] = new RandomAccessFile(partFiles[i], "r");
            byte[] buffer = new byte[BUFFER];
            for (int i = 0; i < files.length(); i++) {
                JSONObject entry = files.getJSONObject(i);
                String path = entry.getString("path");
                if (!path.startsWith("Data/") || path.contains(".."))
                    return "清单路径非法：" + path;
                File target = new File(gameRoot, path);
                long size = entry.getLong("size");
                if (target.isFile() && target.length() == size) {
                    progress.update("已完成 " + (i + 1) + "/" + files.length(),
                            0.35f + 0.65f * (i + 1) / files.length());
                    continue;
                }
                progress.update("展开 " + path, 0.35f + 0.65f * i / files.length());
                File parent = target.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                    return "无法创建目录：" + parent;
                File temporary = new File(target.getPath() + ".partial");
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                JSONArray segments = entry.getJSONArray("segments");
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    for (int s = 0; s < segments.length(); s++) {
                        JSONObject segment = segments.getJSONObject(s);
                        int part = segment.getInt("part");
                        long offset = segment.getLong("offset");
                        long length = segment.getLong("length");
                        handles[part].seek(offset);
                        long left = length;
                        while (left > 0) {
                            int want = (int) Math.min(left, buffer.length);
                            int read = handles[part].read(buffer, 0, want);
                            if (read <= 0) return "分卷读取中断：" + partFiles[part].getName();
                            output.write(buffer, 0, read);
                            digest.update(buffer, 0, read);
                            left -= read;
                        }
                    }
                }
                String actual = hex(digest.digest());
                if (!actual.equalsIgnoreCase(entry.getString("sha256"))) {
                    //noinspection ResultOfMethodCallIgnored
                    temporary.delete();
                    return "文件校验失败：" + path;
                }
                if (!temporary.renameTo(target)) {
                    //noinspection ResultOfMethodCallIgnored
                    temporary.delete();
                    return "无法写入文件：" + path;
                }
            }
        } finally {
            for (RandomAccessFile handle : handles)
                if (handle != null) try { handle.close(); } catch (IOException ignored) { }
        }
        progress.update("资源就绪", 1.0f);
        return null;
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[BUFFER];
        try (FileInputStream input = new FileInputStream(file)) {
            int read;
            while ((read = input.read(buffer)) > 0) digest.update(buffer, 0, read);
        }
        return hex(digest.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) builder.append(String.format("%02x", value));
        return builder.toString();
    }
}
