package org.jetbrains.kotlin

import org.gradle.api.Project
import org.gradle.api.Task

import org.jetbrains.kotlin.konan.target.HostManager
//import org.jetbrains.kotlin.gradle.plugin.konan.KonanArtifactContainer

import java.io.File

fun Project.createStdlibTest(name: String, configure: (KonanGTestRunner) -> Unit = {}): KonanGTestRunner =
        project.tasks.create("execute${name.capitalize()}", KonanGTestRunner::class.java).apply {
            // Configure test task.
            val testOutput = (project.findProperty("testOutputStdlib") as? File)?.toString()
                    ?: throw RuntimeException("Output directory testOutputStdlib is not set")
            val target = project.testTarget()
            executable = "$testOutput/${target.name}/$name.${target.family.exeSuffix}"
            useFilter = false
            testLogger = RunnerLogger.GTEST

            // Configure also
            configure(this)

            // Set dependencies.
            val compileTask = "compileKonan${name.capitalize()}"
            dependsOn(compileTask)
            setDistDependencyFor(compileTask)
            finalizedBy("resultsTask")
        }

fun Project.setDistDependencyFor(taskName: String) {
    project.setDistDependencyFor(project.tasks.getByName(taskName))
}

fun Project.setDistDependencyFor(t: Task) {
    val rootTasks = project.rootProject.tasks
    // We don't build the compiler if a custom dist path is specified.
    if (project.findProperty("useCustomDist") != null) {
        t.dependsOn(rootTasks.getByName("dist"))
        val target = project.testTarget()
        if (target != HostManager.host) {
            // if a test_target property is set then tests should depend on a crossDist
            // otherwise runtime components would not be build for a target.
            t.dependsOn(rootTasks.getByName("${target.name}CrossDist"))
        }
    }
}

fun Project.createStandaloneTest(name: String, configure: (KonanStandaloneTestRunner) -> Unit = {}): KonanStandaloneTestRunner =
        project.tasks.create("execute${name.capitalize()}", KonanStandaloneTestRunner::class.java).apply {
            // Configure test task.
            val testOutput = (project.findProperty("testOutputLocal") as? File)?.toString()
                    ?: throw RuntimeException("Output directory testOutputLocal is not set")
            val target = project.testTarget()

            // Configure
            configure(this)

            // Set executable for the runner.
            val exeName = project.file(source).name.replace(".kt", "")
            executable = "$testOutput/${target.name}/$exeName.${target.family.exeSuffix}"

            // Set dependencies.
            val compileTask = "compileKonan${exeName.capitalize()}"
            dependsOn(compileTask)
            setDistDependencyFor(compileTask)
            finalizedBy("resultsTask")
        }